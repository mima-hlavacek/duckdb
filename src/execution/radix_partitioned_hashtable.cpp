#include "duckdb/execution/radix_partitioned_hashtable.hpp"

#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

namespace duckdb {

RadixPartitionedHashTable::RadixPartitionedHashTable(GroupingSet &grouping_set_p, const GroupedAggregateData &op_p)
    : grouping_set(grouping_set_p), op(op_p) {
	auto groups_count = op.GroupCount();
	for (idx_t i = 0; i < groups_count; i++) {
		if (grouping_set.find(i) == grouping_set.end()) {
			null_groups.push_back(i);
		}
	}
	if (grouping_set.empty()) {
		// Fake a single group with a constant value for aggregation without groups
		group_types.emplace_back(LogicalType::TINYINT);
	}
	for (auto &entry : grouping_set) {
		D_ASSERT(entry < op.group_types.size());
		group_types.push_back(op.group_types[entry]);
	}
	SetGroupingValues();

	auto group_types_copy = group_types;
	group_types_copy.emplace_back(LogicalType::HASH);
	layout.Initialize(std::move(group_types_copy), AggregateObject::CreateAggregateObjects(op.bindings));
}

void RadixPartitionedHashTable::SetGroupingValues() {
	// Compute the GROUPING values:
	// For each parameter to the GROUPING clause, we check if the hash table groups on this particular group
	// If it does, we return 0, otherwise we return 1
	// We then use bitshifts to combine these values
	auto &grouping_functions = op.GetGroupingFunctions();
	for (auto &grouping : grouping_functions) {
		int64_t grouping_value = 0;
		D_ASSERT(grouping.size() < sizeof(int64_t) * 8);
		for (idx_t i = 0; i < grouping.size(); i++) {
			if (grouping_set.find(grouping[i]) == grouping_set.end()) {
				// We don't group on this value!
				grouping_value += (int64_t)1 << (grouping.size() - (i + 1));
			}
		}
		grouping_values.push_back(Value::BIGINT(grouping_value));
	}
}

const TupleDataLayout &RadixPartitionedHashTable::GetLayout() const {
	return layout;
}

unique_ptr<GroupedAggregateHashTable> RadixPartitionedHashTable::CreateHT(ClientContext &context, const idx_t capacity,
                                                                          const idx_t radix_bits) const {
	return make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), group_types, op.payload_types,
	                                            op.bindings, capacity, radix_bits);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
enum class AggregatePartitionState : uint8_t {
	//! Can be finalized
	READY_TO_FINALIZE = 0,
	//! Finalize is in progress
	FINALIZE_IN_PROGRESS = 1,
	//! Finalized, ready to scan
	READY_TO_SCAN = 2
};

struct AggregatePartition {
	explicit AggregatePartition(unique_ptr<TupleDataCollection> data_p)
	    : state(AggregatePartitionState::READY_TO_FINALIZE), data(std::move(data_p)), progress(0) {
	}

	mutex lock;
	AggregatePartitionState state;

	unique_ptr<TupleDataCollection> data;
	atomic<double> progress;

	vector<InterruptState> blocked_tasks;
};

class RadixHTGlobalSinkState;

struct RadixHTConfig {
public:
	explicit RadixHTConfig(ClientContext &context, RadixHTGlobalSinkState &sink);

	void SetRadixBits(idx_t radix_bits_p);
	bool SetRadixBitsToExternal();
	idx_t GetRadixBits() const;

private:
	void SetRadixBitsInternal(const idx_t radix_bits_p, bool external);
	static idx_t InitialSinkRadixBits(ClientContext &context);
	static idx_t MaximumSinkRadixBits(ClientContext &context);
	static idx_t ExternalRadixBits(const idx_t &maximum_sink_radix_bits_p);
	static idx_t SinkCapacity(ClientContext &context);

private:
	//! Assume (1 << 15) = 32KB L1 cache per core, divided by two because hyperthreading
	static constexpr const idx_t L1_CACHE_SIZE = 32768 / 2;
	//! Assume (1 << 20) = 1MB L2 cache per core, divided by two because hyperthreading
	static constexpr const idx_t L2_CACHE_SIZE = 1048576 / 2;
	//! Assume (1 << 20) + (1 << 19) = 1.5MB L3 cache per core (shared), divided by two because hyperthreading
	static constexpr const idx_t L3_CACHE_SIZE = 1572864 / 2;

	//! Sink radix bits to initialize with
	static constexpr const idx_t MAXIMUM_INITIAL_SINK_RADIX_BITS = 3;
	//! Maximum Sink radix bits (independent of threads)
	static constexpr const idx_t MAXIMUM_FINAL_SINK_RADIX_BITS = 7;
	//! By how many radix bits to increment if we go external
	static constexpr const idx_t EXTERNAL_RADIX_BITS_INCREMENT = 3;

	//! The global sink state
	RadixHTGlobalSinkState &sink;
	//! Current thread-global sink radix bits
	atomic<idx_t> sink_radix_bits;
	//! Maximum Sink radix bits (set based on number of threads)
	const idx_t maximum_sink_radix_bits;
	//! Radix bits if we go external
	const idx_t external_radix_bits;

public:
	//! Capacity of HTs during the Sink
	const idx_t sink_capacity;

	//! If we fill this many blocks per partition, we trigger a repartition
	static constexpr const double BLOCK_FILL_FACTOR = 1.8;
	//! By how many bits to repartition if a repartition is triggered
	static constexpr const idx_t REPARTITION_RADIX_BITS = 2;
};

class RadixHTGlobalSinkState : public GlobalSinkState {
public:
	RadixHTGlobalSinkState(ClientContext &context, const RadixPartitionedHashTable &radix_ht);

	//! Destroys aggregate states (if multi-scan)
	~RadixHTGlobalSinkState() override;
	void Destroy();

public:
	ClientContext &context;
	//! Temporary memory state for managing this hash table's memory usage
	unique_ptr<TemporaryMemoryState> temporary_memory_state;

	//! The radix HT
	const RadixPartitionedHashTable &radix_ht;
	//! Config for partitioning
	RadixHTConfig config;

	//! Whether we've called Finalize
	bool finalized;
	//! Whether we are doing an external aggregation
	atomic<bool> external;
	//! Threads that have called Sink
	atomic<idx_t> active_threads;
	//! If any thread has called combine
	atomic<bool> any_combined;

	//! Lock for uncombined_data/stored_allocators
	mutex lock;
	//! Uncombined partitioned data that will be put into the AggregatePartitions
	unique_ptr<PartitionedTupleData> uncombined_data;
	//! Allocators used during the Sink/Finalize
	vector<shared_ptr<ArenaAllocator>> stored_allocators;

	//! Partitions that are finalized during GetData
	vector<unique_ptr<AggregatePartition>> partitions;
	//! For keeping track of progress
	atomic<idx_t> finalize_done;

	//! Pin properties when scanning
	TupleDataPinProperties scan_pin_properties;
	//! Total count before combining
	idx_t count_before_combining;
	//! Maximum partition size if all unique
	idx_t max_partition_size;
};

RadixHTGlobalSinkState::RadixHTGlobalSinkState(ClientContext &context_p, const RadixPartitionedHashTable &radix_ht_p)
    : context(context_p), temporary_memory_state(TemporaryMemoryManager::Get(context).Register(context)),
      radix_ht(radix_ht_p), config(context, *this), finalized(false), external(false), active_threads(0),
      any_combined(false), finalize_done(0), scan_pin_properties(TupleDataPinProperties::DESTROY_AFTER_DONE),
      count_before_combining(0), max_partition_size(0) {

	auto tuples_per_block = Storage::BLOCK_ALLOC_SIZE / radix_ht.GetLayout().GetRowWidth();
	idx_t ht_count = config.sink_capacity / GroupedAggregateHashTable::LOAD_FACTOR;
	auto num_partitions = RadixPartitioning::NumberOfPartitions(config.GetRadixBits());
	auto count_per_partition = ht_count / num_partitions;
	auto blocks_per_partition = (count_per_partition + tuples_per_block) / tuples_per_block + 1;
	auto ht_size = blocks_per_partition * Storage::BLOCK_ALLOC_SIZE + config.sink_capacity * sizeof(aggr_ht_entry_t);

	// This really is the minimum reservation that we can do
	auto num_threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
	auto minimum_reservation = num_threads * ht_size;

	temporary_memory_state->SetMinimumReservation(minimum_reservation);
	temporary_memory_state->SetRemainingSize(context, minimum_reservation);
}

RadixHTGlobalSinkState::~RadixHTGlobalSinkState() {
	Destroy();
}

// LCOV_EXCL_START
void RadixHTGlobalSinkState::Destroy() {
	if (scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE || count_before_combining == 0 ||
	    partitions.empty()) {
		// Already destroyed / empty
		return;
	}

	TupleDataLayout layout = partitions[0]->data->GetLayout().Copy();
	if (!layout.HasDestructor()) {
		return; // No destructors, exit
	}

	// There are aggregates with destructors: Call the destructor for each of the aggregates
	RowOperationsState row_state(*stored_allocators.back());
	for (auto &partition : partitions) {
		auto &data_collection = *partition->data;
		if (data_collection.Count() == 0) {
			continue;
		}
		TupleDataChunkIterator iterator(data_collection, TupleDataPinProperties::DESTROY_AFTER_DONE, false);
		auto &row_locations = iterator.GetChunkState().row_locations;
		do {
			RowOperations::DestroyStates(row_state, layout, row_locations, iterator.GetCurrentChunkCount());
		} while (iterator.Next());
		data_collection.Reset();
	}
}
// LCOV_EXCL_STOP

RadixHTConfig::RadixHTConfig(ClientContext &context, RadixHTGlobalSinkState &sink_p)
    : sink(sink_p), sink_radix_bits(InitialSinkRadixBits(context)),
      maximum_sink_radix_bits(MaximumSinkRadixBits(context)),
      external_radix_bits(ExternalRadixBits(maximum_sink_radix_bits)), sink_capacity(SinkCapacity(context)) {
}

void RadixHTConfig::SetRadixBits(idx_t radix_bits_p) {
	SetRadixBitsInternal(MinValue(radix_bits_p, maximum_sink_radix_bits), false);
}

bool RadixHTConfig::SetRadixBitsToExternal() {
	SetRadixBitsInternal(external_radix_bits, true);
	return sink.external;
}

idx_t RadixHTConfig::GetRadixBits() const {
	return sink_radix_bits;
}

void RadixHTConfig::SetRadixBitsInternal(const idx_t radix_bits_p, bool external) {
	if (sink_radix_bits >= radix_bits_p || sink.any_combined) {
		return;
	}

	lock_guard<mutex> guard(sink.lock);
	if (sink_radix_bits >= radix_bits_p || sink.any_combined) {
		return;
	}

	if (external) {
		sink.external = true;
	}
	sink_radix_bits = radix_bits_p;
	return;
}

idx_t RadixHTConfig::InitialSinkRadixBits(ClientContext &context) {
	const auto active_threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
	return MinValue(RadixPartitioning::RadixBits(NextPowerOfTwo(active_threads)), MAXIMUM_INITIAL_SINK_RADIX_BITS);
}

idx_t RadixHTConfig::MaximumSinkRadixBits(ClientContext &context) {
	const auto active_threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
	return MinValue(RadixPartitioning::RadixBits(NextPowerOfTwo(active_threads)), MAXIMUM_FINAL_SINK_RADIX_BITS);
}

idx_t RadixHTConfig::ExternalRadixBits(const idx_t &maximum_sink_radix_bits_p) {
	return MinValue(maximum_sink_radix_bits_p + EXTERNAL_RADIX_BITS_INCREMENT, MAXIMUM_FINAL_SINK_RADIX_BITS);
}

idx_t RadixHTConfig::SinkCapacity(ClientContext &context) {
	// Get active and maximum number of threads
	const auto active_threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());

	// Compute cache size per active thread (assuming cache is shared)
	const auto total_shared_cache_size = active_threads * L3_CACHE_SIZE;
	const auto cache_per_active_thread = L1_CACHE_SIZE + L2_CACHE_SIZE + total_shared_cache_size / active_threads;

	// Divide cache per active thread by entry size, round up to next power of two, to get capacity
	const auto size_per_entry = sizeof(aggr_ht_entry_t) * GroupedAggregateHashTable::LOAD_FACTOR;
	const auto capacity = NextPowerOfTwo(cache_per_active_thread / size_per_entry);

	// Capacity must be at least the minimum capacity
	return MaxValue<idx_t>(capacity, GroupedAggregateHashTable::InitialCapacity());
}

class RadixHTLocalSinkState : public LocalSinkState {
public:
	RadixHTLocalSinkState(ClientContext &context, const RadixPartitionedHashTable &radix_ht);

public:
	//! Thread-local HT that is re-used after abandoning
	unique_ptr<GroupedAggregateHashTable> ht;
	//! Chunk with group columns
	DataChunk group_chunk;

	//! Data that is abandoned ends up here (only if we're doing external aggregation)
	unique_ptr<PartitionedTupleData> abandoned_data;
};

RadixHTLocalSinkState::RadixHTLocalSinkState(ClientContext &, const RadixPartitionedHashTable &radix_ht) {
	// If there are no groups we create a fake group so everything has the same group
	group_chunk.InitializeEmpty(radix_ht.group_types);
	if (radix_ht.grouping_set.empty()) {
		group_chunk.data[0].Reference(Value::TINYINT(42));
	}
}

unique_ptr<GlobalSinkState> RadixPartitionedHashTable::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<RadixHTGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> RadixPartitionedHashTable::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<RadixHTLocalSinkState>(context.client, *this);
}

void RadixPartitionedHashTable::PopulateGroupChunk(DataChunk &group_chunk, DataChunk &input_chunk) const {
	idx_t chunk_index = 0;
	// Populate the group_chunk
	for (auto &group_idx : grouping_set) {
		// Retrieve the expression containing the index in the input chunk
		auto &group = op.groups[group_idx];
		D_ASSERT(group->type == ExpressionType::BOUND_REF);
		auto &bound_ref_expr = group->Cast<BoundReferenceExpression>();
		// Reference from input_chunk[group.index] -> group_chunk[chunk_index]
		group_chunk.data[chunk_index++].Reference(input_chunk.data[bound_ref_expr.index]);
	}
	group_chunk.SetCardinality(input_chunk.size());
	group_chunk.Verify();
}

bool MaybeRepartition(ClientContext &context, RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate,
                      const idx_t &active_threads) {
	auto &config = gstate.config;
	auto &ht = *lstate.ht;
	auto &partitioned_data = ht.GetPartitionedData();

	// Check if we're approaching the memory limit
	auto &temporary_memory_state = *gstate.temporary_memory_state;
	const auto total_size = partitioned_data->SizeInBytes() + ht.Capacity() * sizeof(aggr_ht_entry_t);
	idx_t thread_limit = temporary_memory_state.GetReservation() / active_threads;
	if (total_size > thread_limit) {
		// We're over the thread memory limit
		if (!gstate.external) {
			// We haven't yet triggered out-of-core behavior, but maybe we don't have to, grab the lock and check again
			lock_guard<mutex> guard(gstate.lock);
			thread_limit = temporary_memory_state.GetReservation() / active_threads;
			if (total_size > thread_limit) {
				// Out-of-core would be triggered below, try to increase the reservation
				auto remaining_size =
				    MaxValue<idx_t>(active_threads * total_size, temporary_memory_state.GetRemainingSize());
				temporary_memory_state.SetRemainingSize(context, 2 * remaining_size);
				thread_limit = temporary_memory_state.GetReservation() / active_threads;
			}
		}
	}

	if (total_size > thread_limit) {
		if (gstate.config.SetRadixBitsToExternal()) {
			// We're approaching the memory limit, unpin the data
			if (!lstate.abandoned_data) {
				lstate.abandoned_data = make_uniq<RadixPartitionedTupleData>(
				    BufferManager::GetBufferManager(context), gstate.radix_ht.GetLayout(), config.GetRadixBits(),
				    gstate.radix_ht.GetLayout().ColumnCount() - 1);
			}

			ht.UnpinData();
			partitioned_data->Repartition(*lstate.abandoned_data);
			ht.SetRadixBits(gstate.config.GetRadixBits());
			ht.InitializePartitionedData();
			return true;
		}
	}

	// We can go external when there is only one active thread, but we shouldn't repartition here
	if (active_threads < 2) {
		return false;
	}

	const auto partition_count = partitioned_data->PartitionCount();
	const auto current_radix_bits = RadixPartitioning::RadixBits(partition_count);
	D_ASSERT(current_radix_bits <= config.GetRadixBits());

	const auto row_size_per_partition =
	    partitioned_data->Count() * partitioned_data->GetLayout().GetRowWidth() / partition_count;
	if (row_size_per_partition > config.BLOCK_FILL_FACTOR * Storage::BLOCK_SIZE) {
		// We crossed our block filling threshold, try to increment radix bits
		config.SetRadixBits(current_radix_bits + config.REPARTITION_RADIX_BITS);
	}

	const auto global_radix_bits = config.GetRadixBits();
	if (current_radix_bits == global_radix_bits) {
		return false; // We're already on the right number of radix bits
	}

	// We're out-of-sync with the global radix bits, repartition
	ht.UnpinData();
	auto old_partitioned_data = std::move(partitioned_data);
	ht.SetRadixBits(global_radix_bits);
	ht.InitializePartitionedData();
	old_partitioned_data->Repartition(*ht.GetPartitionedData());
	return true;
}

void RadixPartitionedHashTable::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input,
                                     DataChunk &payload_input, const unsafe_vector<idx_t> &filter) const {
	auto &gstate = input.global_state.Cast<RadixHTGlobalSinkState>();
	auto &lstate = input.local_state.Cast<RadixHTLocalSinkState>();
	if (!lstate.ht) {
		lstate.ht = CreateHT(context.client, gstate.config.sink_capacity, gstate.config.GetRadixBits());
		gstate.active_threads++;
	}

	auto &group_chunk = lstate.group_chunk;
	PopulateGroupChunk(group_chunk, chunk);

	auto &ht = *lstate.ht;
	ht.AddChunk(group_chunk, payload_input, filter);

	if (ht.Count() + STANDARD_VECTOR_SIZE < ht.ResizeThreshold()) {
		return; // We can fit another chunk
	}

	const idx_t active_threads = gstate.active_threads;
	if (active_threads > 2) {
		// 'Reset' the HT without taking its data, we can just keep appending to the same collection
		// This only works because we never resize the HT
		ht.ClearPointerTable();
		ht.ResetCount();
		// We don't do this when running with 1 or 2 threads, it only makes sense when there's many threads
	}

	// Check if we need to repartition
	auto repartitioned = MaybeRepartition(context.client, gstate, lstate, active_threads);

	if (repartitioned && ht.Count() != 0) {
		// We repartitioned, but we didn't clear the pointer table / reset the count because we're on 1 or 2 threads
		ht.ClearPointerTable();
		ht.ResetCount();
	}

	// TODO: combine early and often
}

void RadixPartitionedHashTable::Combine(ExecutionContext &context, GlobalSinkState &gstate_p,
                                        LocalSinkState &lstate_p) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSinkState>();
	auto &lstate = lstate_p.Cast<RadixHTLocalSinkState>();
	if (!lstate.ht) {
		return;
	}

	// Set any_combined, then check one last time whether we need to repartition
	gstate.any_combined = true;
	MaybeRepartition(context.client, gstate, lstate, gstate.active_threads);

	auto &ht = *lstate.ht;
	ht.UnpinData();

	if (lstate.abandoned_data) {
		D_ASSERT(gstate.external);
		D_ASSERT(lstate.abandoned_data->PartitionCount() == lstate.ht->GetPartitionedData()->PartitionCount());
		D_ASSERT(lstate.abandoned_data->PartitionCount() ==
		         RadixPartitioning::NumberOfPartitions(gstate.config.GetRadixBits()));
		lstate.abandoned_data->Combine(*lstate.ht->GetPartitionedData());
	} else {
		lstate.abandoned_data = std::move(ht.GetPartitionedData());
	}

	lock_guard<mutex> guard(gstate.lock);
	if (gstate.uncombined_data) {
		gstate.uncombined_data->Combine(*lstate.abandoned_data);
	} else {
		gstate.uncombined_data = std::move(lstate.abandoned_data);
	}
	gstate.stored_allocators.emplace_back(ht.GetAggregateAllocator());
}

void RadixPartitionedHashTable::Finalize(ClientContext &context, GlobalSinkState &gstate_p) const {
	auto &gstate = gstate_p.Cast<RadixHTGlobalSinkState>();

	if (gstate.uncombined_data) {
		auto &uncombined_data = *gstate.uncombined_data;
		gstate.count_before_combining = uncombined_data.Count();

		// If true there is no need to combine, it was all done by a single thread in a single HT
		const auto single_ht = !gstate.external && gstate.active_threads == 1;

		auto &uncombined_partition_data = uncombined_data.GetPartitions();
		const auto n_partitions = uncombined_partition_data.size();
		gstate.partitions.reserve(n_partitions);
		for (idx_t i = 0; i < n_partitions; i++) {
			auto &partition = uncombined_partition_data[i];
			auto partition_size =
			    partition->SizeInBytes() +
			    GroupedAggregateHashTable::GetCapacityForCount(partition->Count()) * sizeof(aggr_ht_entry_t);
			gstate.max_partition_size = MaxValue(gstate.max_partition_size, partition_size);

			gstate.partitions.emplace_back(make_uniq<AggregatePartition>(std::move(partition)));
			if (single_ht) {
				gstate.finalize_done++;
				gstate.partitions.back()->progress = 1;
				gstate.partitions.back()->state = AggregatePartitionState::READY_TO_SCAN;
			}
		}
	} else {
		gstate.count_before_combining = 0;
	}

	// Minimum of combining one partition at a time
	gstate.temporary_memory_state->SetMinimumReservation(gstate.max_partition_size);
	// Maximum of combining all partitions
	auto max_threads = MinValue<idx_t>(NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads()),
	                                   gstate.partitions.size());
	gstate.temporary_memory_state->SetRemainingSize(context, max_threads * gstate.max_partition_size);
	gstate.finalized = true;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
idx_t RadixPartitionedHashTable::MaxThreads(GlobalSinkState &sink_p) const {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	if (sink.partitions.empty()) {
		return 0;
	}

	// This many partitions will fit given our reservation (at least 1))
	auto partitions_fit = MaxValue<idx_t>(sink.temporary_memory_state->GetReservation() / sink.max_partition_size, 1);
	// Maximum is either the number of partitions, or the number of threads
	auto max_possible = MinValue<idx_t>(
	    sink.partitions.size(), NumericCast<idx_t>(TaskScheduler::GetScheduler(sink.context).NumberOfThreads()));

	// Mininum of the two
	return MinValue<idx_t>(partitions_fit, max_possible);
}

void RadixPartitionedHashTable::SetMultiScan(GlobalSinkState &sink_p) {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	sink.scan_pin_properties = TupleDataPinProperties::UNPIN_AFTER_DONE;
}

enum class RadixHTSourceTaskType : uint8_t { NO_TASK, FINALIZE, SCAN };

class RadixHTLocalSourceState;

class RadixHTGlobalSourceState : public GlobalSourceState {
public:
	RadixHTGlobalSourceState(ClientContext &context, const RadixPartitionedHashTable &radix_ht);

	//! Assigns a task to a local source state
	SourceResultType AssignTask(RadixHTGlobalSinkState &sink, RadixHTLocalSourceState &lstate,
	                            InterruptState &interrupt_state);

public:
	//! The client context
	ClientContext &context;
	//! For synchronizing the source phase
	atomic<bool> finished;

	//! Column ids for scanning
	vector<column_t> column_ids;

	//! For synchronizing tasks
	mutex lock;
	idx_t task_idx;
	atomic<idx_t> task_done;
};

enum class RadixHTScanStatus : uint8_t { INIT, IN_PROGRESS, DONE };

class RadixHTLocalSourceState : public LocalSourceState {
public:
	explicit RadixHTLocalSourceState(ExecutionContext &context, const RadixPartitionedHashTable &radix_ht);

public:
	//! Do the work this thread has been assigned
	void ExecuteTask(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate, DataChunk &chunk);
	//! Whether this thread has finished the work it has been assigned
	bool TaskFinished();

private:
	//! Execute the finalize or scan task
	void Finalize(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate);
	void Scan(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate, DataChunk &chunk);

public:
	//! Current task and index
	RadixHTSourceTaskType task;
	idx_t task_idx;

	//! Thread-local HT that is re-used to Finalize
	unique_ptr<GroupedAggregateHashTable> ht;
	//! Current status of a Scan
	RadixHTScanStatus scan_status;

private:
	//! Allocator and layout for finalizing state
	TupleDataLayout layout;
	ArenaAllocator aggregate_allocator;

	//! State and chunk for scanning
	TupleDataScanState scan_state;
	DataChunk scan_chunk;
};

unique_ptr<GlobalSourceState> RadixPartitionedHashTable::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<RadixHTGlobalSourceState>(context, *this);
}

unique_ptr<LocalSourceState> RadixPartitionedHashTable::GetLocalSourceState(ExecutionContext &context) const {
	return make_uniq<RadixHTLocalSourceState>(context, *this);
}

RadixHTGlobalSourceState::RadixHTGlobalSourceState(ClientContext &context_p, const RadixPartitionedHashTable &radix_ht)
    : context(context_p), finished(false), task_idx(0), task_done(0) {
	for (column_t column_id = 0; column_id < radix_ht.group_types.size(); column_id++) {
		column_ids.push_back(column_id);
	}
}

SourceResultType RadixHTGlobalSourceState::AssignTask(RadixHTGlobalSinkState &sink, RadixHTLocalSourceState &lstate,
                                                      InterruptState &interrupt_state) {
	// First, try to get a partition index
	lock_guard<mutex> gstate_guard(lock);
	if (finished) {
		return SourceResultType::FINISHED;
	}
	if (task_idx == sink.partitions.size()) {
		return SourceResultType::FINISHED;
	}
	lstate.task_idx = task_idx++;

	// We got a partition index
	auto &partition = *sink.partitions[lstate.task_idx];
	auto partition_lock = unique_lock<mutex>(partition.lock);
	switch (partition.state) {
	case AggregatePartitionState::READY_TO_FINALIZE:
		partition.state = AggregatePartitionState::FINALIZE_IN_PROGRESS;
		lstate.task = RadixHTSourceTaskType::FINALIZE;
		return SourceResultType::HAVE_MORE_OUTPUT;
	case AggregatePartitionState::FINALIZE_IN_PROGRESS:
		lstate.task = RadixHTSourceTaskType::SCAN;
		lstate.scan_status = RadixHTScanStatus::INIT;
		partition.blocked_tasks.push_back(interrupt_state);
		return SourceResultType::BLOCKED;
	case AggregatePartitionState::READY_TO_SCAN:
		lstate.task = RadixHTSourceTaskType::SCAN;
		lstate.scan_status = RadixHTScanStatus::INIT;
		return SourceResultType::HAVE_MORE_OUTPUT;
	default:
		throw InternalException("Unexpected AggregatePartitionState in RadixHTLocalSourceState::Finalize!");
	}
}

RadixHTLocalSourceState::RadixHTLocalSourceState(ExecutionContext &context, const RadixPartitionedHashTable &radix_ht)
    : task(RadixHTSourceTaskType::NO_TASK), scan_status(RadixHTScanStatus::DONE), layout(radix_ht.GetLayout().Copy()),
      aggregate_allocator(BufferAllocator::Get(context.client)) {
	auto &allocator = BufferAllocator::Get(context.client);
	auto scan_chunk_types = radix_ht.group_types;
	for (auto &aggr_type : radix_ht.op.aggregate_return_types) {
		scan_chunk_types.push_back(aggr_type);
	}
	scan_chunk.Initialize(allocator, scan_chunk_types);
}

void RadixHTLocalSourceState::ExecuteTask(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate,
                                          DataChunk &chunk) {
	D_ASSERT(task != RadixHTSourceTaskType::NO_TASK);
	switch (task) {
	case RadixHTSourceTaskType::FINALIZE:
		Finalize(sink, gstate);
		break;
	case RadixHTSourceTaskType::SCAN:
		Scan(sink, gstate, chunk);
		break;
	default:
		throw InternalException("Unexpected RadixHTSourceTaskType in ExecuteTask!");
	}
}

void RadixHTLocalSourceState::Finalize(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate) {
	D_ASSERT(task == RadixHTSourceTaskType::FINALIZE);
	D_ASSERT(scan_status != RadixHTScanStatus::IN_PROGRESS);
	auto &partition = *sink.partitions[task_idx];

	if (!ht) {
		// This capacity would always be sufficient for all data
		const auto capacity = GroupedAggregateHashTable::GetCapacityForCount(partition.data->Count());

		// However, we will limit the initial capacity so we don't do a huge over-allocation
		const auto n_threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(gstate.context).NumberOfThreads());
		const auto memory_limit = BufferManager::GetBufferManager(gstate.context).GetMaxMemory();
		const idx_t thread_limit = 0.6 * memory_limit / n_threads;

		const idx_t size_per_entry = partition.data->SizeInBytes() / MaxValue<idx_t>(partition.data->Count(), 1) +
		                             idx_t(GroupedAggregateHashTable::LOAD_FACTOR * sizeof(aggr_ht_entry_t));
		const auto capacity_limit = NextPowerOfTwo(thread_limit / size_per_entry);

		ht = sink.radix_ht.CreateHT(gstate.context, MinValue<idx_t>(capacity, capacity_limit), 0);
	} else {
		// We may want to resize here to the size of this partition, but for now we just assume uniform partition sizes
		ht->InitializePartitionedData();
		ht->ClearPointerTable();
		ht->ResetCount();
	}

	// Now combine the uncombined data using this thread's HT
	ht->Combine(*partition.data, &partition.progress);
	ht->UnpinData();
	partition.progress = 1;

	// Move the combined data back to the partition
	partition.data =
	    make_uniq<TupleDataCollection>(BufferManager::GetBufferManager(gstate.context), sink.radix_ht.GetLayout());
	partition.data->Combine(*ht->GetPartitionedData()->GetPartitions()[0]);

	// Update thread-global state
	lock_guard<mutex> global_guard(gstate.lock);
	sink.stored_allocators.emplace_back(ht->GetAggregateAllocator());
	const auto finalizes_done = ++sink.finalize_done;
	D_ASSERT(finalizes_done <= sink.partitions.size());
	if (finalizes_done == sink.partitions.size()) {
		// All finalizes are done, set remaining size to 0
		sink.temporary_memory_state->SetRemainingSize(sink.context, 0);
	}

	// Update partition state
	lock_guard<mutex> partition_guard(partition.lock);
	partition.state = AggregatePartitionState::READY_TO_SCAN;
	for (auto &blocked_task : partition.blocked_tasks) {
		blocked_task.Callback();
	}
	partition.blocked_tasks.clear();

	// This thread will scan the partition
	task = RadixHTSourceTaskType::SCAN;
	scan_status = RadixHTScanStatus::INIT;
}

void RadixHTLocalSourceState::Scan(RadixHTGlobalSinkState &sink, RadixHTGlobalSourceState &gstate, DataChunk &chunk) {
	D_ASSERT(task == RadixHTSourceTaskType::SCAN);
	D_ASSERT(scan_status != RadixHTScanStatus::DONE);

	auto &partition = *sink.partitions[task_idx];
	D_ASSERT(partition.state == AggregatePartitionState::READY_TO_SCAN);
	auto &data_collection = *partition.data;

	if (scan_status == RadixHTScanStatus::INIT) {
		data_collection.InitializeScan(scan_state, gstate.column_ids, sink.scan_pin_properties);
		scan_status = RadixHTScanStatus::IN_PROGRESS;
	}

	if (!data_collection.Scan(scan_state, scan_chunk)) {
		if (sink.scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE) {
			data_collection.Reset();
		}
		scan_status = RadixHTScanStatus::DONE;
		lock_guard<mutex> gstate_guard(gstate.lock);
		if (++gstate.task_done == sink.partitions.size()) {
			gstate.finished = true;
		}
		return;
	}

	RowOperationsState row_state(aggregate_allocator);
	const auto group_cols = layout.ColumnCount() - 1;
	RowOperations::FinalizeStates(row_state, layout, scan_state.chunk_state.row_locations, scan_chunk, group_cols);

	if (sink.scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE && layout.HasDestructor()) {
		RowOperations::DestroyStates(row_state, layout, scan_state.chunk_state.row_locations, scan_chunk.size());
	}

	auto &radix_ht = sink.radix_ht;
	idx_t chunk_index = 0;
	for (auto &entry : radix_ht.grouping_set) {
		chunk.data[entry].Reference(scan_chunk.data[chunk_index++]);
	}
	for (auto null_group : radix_ht.null_groups) {
		chunk.data[null_group].SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(chunk.data[null_group], true);
	}
	D_ASSERT(radix_ht.grouping_set.size() + radix_ht.null_groups.size() == radix_ht.op.GroupCount());
	for (idx_t col_idx = 0; col_idx < radix_ht.op.aggregates.size(); col_idx++) {
		chunk.data[radix_ht.op.GroupCount() + col_idx].Reference(
		    scan_chunk.data[radix_ht.group_types.size() + col_idx]);
	}
	D_ASSERT(radix_ht.op.grouping_functions.size() == radix_ht.grouping_values.size());
	for (idx_t i = 0; i < radix_ht.op.grouping_functions.size(); i++) {
		chunk.data[radix_ht.op.GroupCount() + radix_ht.op.aggregates.size() + i].Reference(radix_ht.grouping_values[i]);
	}
	chunk.SetCardinality(scan_chunk);
	D_ASSERT(chunk.size() != 0);
}

bool RadixHTLocalSourceState::TaskFinished() {
	switch (task) {
	case RadixHTSourceTaskType::FINALIZE:
		return true;
	case RadixHTSourceTaskType::SCAN:
		return scan_status == RadixHTScanStatus::DONE;
	default:
		D_ASSERT(task == RadixHTSourceTaskType::NO_TASK);
		return true;
	}
}

SourceResultType RadixPartitionedHashTable::GetData(ExecutionContext &context, DataChunk &chunk,
                                                    GlobalSinkState &sink_p, OperatorSourceInput &input) const {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	D_ASSERT(sink.finalized);

	auto &gstate = input.global_state.Cast<RadixHTGlobalSourceState>();
	auto &lstate = input.local_state.Cast<RadixHTLocalSourceState>();
	D_ASSERT(sink.scan_pin_properties == TupleDataPinProperties::UNPIN_AFTER_DONE ||
	         sink.scan_pin_properties == TupleDataPinProperties::DESTROY_AFTER_DONE);

	if (gstate.finished) {
		return SourceResultType::FINISHED;
	}

	if (sink.count_before_combining == 0) {
		if (grouping_set.empty()) {
			// Special case hack to sort out aggregating from empty intermediates for aggregations without groups
			D_ASSERT(chunk.ColumnCount() == null_groups.size() + op.aggregates.size() + op.grouping_functions.size());
			// For each column in the aggregates, set to initial state
			chunk.SetCardinality(1);
			for (auto null_group : null_groups) {
				chunk.data[null_group].SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(chunk.data[null_group], true);
			}
			ArenaAllocator allocator(BufferAllocator::Get(context.client));
			for (idx_t i = 0; i < op.aggregates.size(); i++) {
				D_ASSERT(op.aggregates[i]->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
				auto &aggr = op.aggregates[i]->Cast<BoundAggregateExpression>();
				auto aggr_state = make_unsafe_uniq_array<data_t>(aggr.function.state_size());
				aggr.function.initialize(aggr_state.get());

				AggregateInputData aggr_input_data(aggr.bind_info.get(), allocator);
				Vector state_vector(Value::POINTER(CastPointerToValue(aggr_state.get())));
				aggr.function.finalize(state_vector, aggr_input_data, chunk.data[null_groups.size() + i], 1, 0);
				if (aggr.function.destructor) {
					aggr.function.destructor(state_vector, aggr_input_data, 1);
				}
			}
			// Place the grouping values (all the groups of the grouping_set condensed into a single value)
			// Behind the null groups + aggregates
			for (idx_t i = 0; i < op.grouping_functions.size(); i++) {
				chunk.data[null_groups.size() + op.aggregates.size() + i].Reference(grouping_values[i]);
			}
		}
		gstate.finished = true;
		return SourceResultType::FINISHED;
	}

	while (!gstate.finished && chunk.size() == 0) {
		if (lstate.TaskFinished()) {
			const auto res = gstate.AssignTask(sink, lstate, input.interrupt_state);
			if (res != SourceResultType::HAVE_MORE_OUTPUT) {
				D_ASSERT(res == SourceResultType::FINISHED || res == SourceResultType::BLOCKED);
				return res;
			}
		}
		lstate.ExecuteTask(sink, gstate, chunk);
	}

	if (chunk.size() != 0) {
		return SourceResultType::HAVE_MORE_OUTPUT;
	} else {
		return SourceResultType::FINISHED;
	}
}

double RadixPartitionedHashTable::GetProgress(ClientContext &, GlobalSinkState &sink_p,
                                              GlobalSourceState &gstate_p) const {
	auto &sink = sink_p.Cast<RadixHTGlobalSinkState>();
	auto &gstate = gstate_p.Cast<RadixHTGlobalSourceState>();

	// Get partition combine progress, weigh it 2x
	double total_progress = 0;
	for (auto &partition : sink.partitions) {
		total_progress += 2.0 * partition->progress;
	}

	// Get scan progress, weigh it 1x
	total_progress += 1.0 * gstate.task_done;

	// Divide by 3x for the weights, and the number of partitions to get a value between 0 and 1 again
	total_progress /= 3.0 * sink.partitions.size();

	// Multiply by 100 to get a percentage
	return 100.0 * total_progress;
}

} // namespace duckdb
