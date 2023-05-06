//
// Created by jinhai on 23-5-1.
//

#define ANKERL_NANOBENCH_IMPLEMENT

#include <thread>
#include "nanobench.h"

#include "common/types/internal_types.h"
#include "faiss/index_factory.h"
#include "main/profiler/base_profiler.h"
#include "faiss/IndexFlat.h"
#include "helper.h"
#include "faiss/IndexIVFFlat.h"
#include "faiss/index_io.h"
#include "scheduler.h"

using namespace infinity;

static const char* sift1m_base = "/home/jinhai/Documents/data/sift1M/sift_base.fvecs";
static const char* sift1m_train = "/home/jinhai/Documents/data/sift1M/sift_learn.fvecs";
static const char* sift1m_query = "/home/jinhai/Documents/data/sift1M/sift_query.fvecs";
static const char* sift1m_ground_truth = "/home/jinhai/Documents/data/sift1M/sift_groundtruth.ivecs";
static const char* ivf_index_name = "ivf_index.bin";
static const char* flat_index_name = "flat_index.bin";

void
batch_run_on_cpu(faiss::Index* index, float* query_vectors, SizeT query_count, SizeT dimension, SizeT top_k, const int* ground_truth) {
    faiss::idx_t* I = new faiss::idx_t[query_count * top_k];
    float* D = new float[query_count * top_k];

    index->search(query_count, query_vectors, top_k, D, I);

    // evaluate result by hand.
    int n_1 = 0, n_10 = 0, n_100 = 0;
    for (int i = 0; i < query_count; i++) {
        int gt_nn = ground_truth[i * top_k];
        for (int j = 0; j < top_k; j++) {
            if (I[i * top_k + j] == gt_nn) {
                if (j < 1)
                    n_1++;
                if (j < 10)
                    n_10++;
                if (j < 100)
                    n_100++;
            }
        }
    }
    printf("R@1 = %.4f\n", n_1 / float(query_count));
    printf("R@10 = %.4f\n", n_10 / float(query_count));
    printf("R@100 = %.4f\n", n_100 / float(query_count));

    delete[] I;
    delete[] D;
}

void
single_run_on_cpu(faiss::Index* index, float* query_vectors, SizeT query_count, SizeT dimension, SizeT top_k, const int* ground_truth) {
    std::cout << "Begin to search " << query_count << " query, memory: "
              << get_current_rss() / 1000000 << " MB" << std::endl;

    infinity::BaseProfiler profiler;
    profiler.Begin();

    faiss::idx_t* I = new faiss::idx_t[top_k];
    float* D = new float[top_k];

    // evaluate result by hand.
    int n_1 = 0, n_10 = 0, n_100 = 0;
    for(SizeT query_index = 0; query_index < query_count; ++  query_index) {
        float* query_vector = &query_vectors[dimension * query_index];
        index->search(1, query_vector, top_k, D, I);

        int gt_nn = ground_truth[query_index * top_k];
        for (int j = 0; j < top_k; j++) {
            if (I[j] == gt_nn) {
                if (j < 1)
                    n_1++;
                if (j < 10)
                    n_10++;
                if (j < 100)
                    n_100++;
            }
        }

        if (query_index % 100 == 0) {
            std::cout << query_index << ", " << get_current_rss() / 1000000 << " MB, "
                      << profiler.ElapsedToString() << std::endl;
//            std::cout << n_1 << " " << n_10 << " " << n_100 << std::endl;
        }
    }

    profiler.End();
    std::cout << "Search: " << profiler.ElapsedToString() << ", "
              << get_current_rss() / 1000000 << " MB" << std::endl;

    printf("R@1 = %.4f\n", n_1 / float(query_count));
    printf("R@10 = %.4f\n", n_10 / float(query_count));
    printf("R@100 = %.4f\n", n_100 / float(query_count));

    delete[] I;
    delete[] D;
}


struct AnnFlatTask : public Task {
    inline explicit
    AnnFlatTask(faiss::Index* index,
                SizeT query_count,
                f32* query_vectors,
                SizeT top_k,
                i64*& result_id,
                f32*& result_vector)
            : Task(TaskType::kAnnFlat),
              index_(index),
              query_count_(query_count),
              query_vectors_(query_vectors),
              top_k_(top_k),
              result_id_(result_id),
              result_vector_(result_vector)
    {}

    void
    run(i64 worker_id) override {
        infinity::BaseProfiler profiler;
        profiler.Begin();
        index_->search(query_count_, query_vectors_, top_k_, result_vector_, result_id_);
        profiler.End();
//        std::cout << "Search: " << profiler.ElapsedToString() << ", "
//                  << get_current_rss() / 1000000 << " MB" << std::endl;
        printf("Run AnnFlat by worker: %ld, spend: %s\n", worker_id, profiler.ElapsedToString().c_str());
//        usleep(1000 * 1000);
    }

private:
    faiss::Index* index_{nullptr};
    SizeT query_count_{0};
    f32*  query_vectors_{nullptr};
    SizeT top_k_{0};
    i64*  result_id_{nullptr}; // I
    f32*  result_vector_{nullptr}; // D
};

void
scheduler_run_on_cpu(faiss::Index* index,
                     float* total_query_vectors,
                     SizeT total_query_count,
                     SizeT dimension,
                     SizeT top_k,
                     const i32* ground_truth) {
    const HashSet<i64> cpu_mask{1, 3, 5, 7, 9, 11, 13, 15};

    i64 cpu_count = std::thread::hardware_concurrency();
    HashSet<i64> cpu_set;
    for(i64 idx = 0; idx < cpu_count; ++ idx) {
        if(!cpu_mask.contains(idx)) {
            cpu_set.insert(idx);
        }
    }

    SizeT task_count = cpu_set.size();
    if(task_count == 0) {
        assert(false || ! "No cpu available");
    }
    SizeT base_task_size = total_query_count / task_count;
    SizeT reminder_size = total_query_count % task_count;
    SizeT query_offset = 0;
    Vector<UniquePtr<AnnFlatTask>> tasks;
    tasks.reserve(task_count);

    i64* total_result_ids = new faiss::idx_t[total_query_count * top_k];
    f32* total_result_vectors = new float[total_query_count * top_k];

    for(SizeT idx = 0; idx < task_count; ++ idx) {
        SizeT query_count = base_task_size;
        if(reminder_size > 0) {
            ++ query_count;
            -- reminder_size;
        }

        f32* query_vectors = &total_query_vectors[query_offset * dimension];


        i64* result_ids = &total_result_ids[query_offset * top_k];
        f32* result_vectors = &total_result_vectors[query_offset * top_k];
        tasks.emplace_back(MakeUnique<AnnFlatTask>(index,
                                                   query_count,
                                                   query_vectors,
                                                   top_k,
                                                   result_ids,
                                                   result_vectors));
        query_offset += query_count;
    }

    Scheduler scheduler;
    scheduler.Init(cpu_set);
    i64 task_id{0};
    for(i64 cpu_id: cpu_set) {
        scheduler.ScheduleTask(cpu_id, tasks[task_id].get());
        ++ task_id;
    }

    scheduler.Uninit();

    int n_1 = 0, n_10 = 0, n_100 = 0;
    for (int i = 0; i < total_query_count; i++) {
        int gt_nn = ground_truth[i * top_k];
        for (int j = 0; j < top_k; j++) {
            if (total_result_ids[i * top_k + j] == gt_nn) {
                if (j < 1)
                    n_1++;
                if (j < 10)
                    n_10++;
                if (j < 100)
                    n_100++;
            }
        }
    }
    printf("R@1 = %.4f\n", n_1 / float(total_query_count));
    printf("R@10 = %.4f\n", n_10 / float(total_query_count));
    printf("R@100 = %.4f\n", n_100 / float(total_query_count));

    delete[] total_result_ids;
    delete[] total_result_vectors;
}

static float*
float_vector_read(const char* fname, size_t* dimension, size_t* row_count, const char* comment) {
    infinity::BaseProfiler profiler;
    profiler.Begin();
    float* float_vec = fvecs_read(fname, dimension, row_count);
    profiler.End();
    if(comment != nullptr) {
        std::cout << "Load " << comment << ": " << profiler.ElapsedToString() << ", "
                  << get_current_rss() / 1000000 << " MB" << std::endl;
    }
    return float_vec;
}

static int*
integer_vector_read(const char* fname, size_t* dimension, size_t* row_count, const char* comment) {
    infinity::BaseProfiler profiler;
    profiler.Begin();
    int* float_vec = ivecs_read(fname, dimension, row_count);
    profiler.End();
    if(comment != nullptr) {
        std::cout << "Load " << comment << ": " << profiler.ElapsedToString() << ", "
                  << get_current_rss() / 1000000 << " MB" << std::endl;
    }
    return float_vec;
}

void
benchmark_flat() {
    faiss::Index* index;
    int dimension = 128;
    {
        // Construct index
        infinity::BaseProfiler profiler;
        profiler.Begin();
        index = new faiss::IndexFlatL2(dimension);
        profiler.End();
        std::cout << "Create Flat index: " << profiler.ElapsedToString() << ", "
                  << get_current_rss() / 1000000 << " MB" << std::endl;
    }

    std::ifstream f(flat_index_name);
    if(f.good()) {
        // Found index file
        std::cout << "Found flat index file ... " << std::endl;
        index = faiss::read_index(flat_index_name);
    } else {
        // Read base vector data
        SizeT base_vector_row_count{0};
        SizeT base_vector_dimension{0};
        float* base_vectors{nullptr};
        {
            base_vectors = float_vector_read(sift1m_base, &base_vector_dimension, &base_vector_row_count, "Base Vector");
        }

        {
            // Build index
            infinity::BaseProfiler profiler;
            profiler.Begin();
            index->add(base_vector_row_count, base_vectors);
            profiler.End();
            std::cout << "Insert Base Vector into index: " << profiler.ElapsedToString() << ", "
                      << get_current_rss() / 1000000 << " MB" << std::endl;
        }

        delete[] base_vectors;

        faiss::write_index(index, flat_index_name);
    }

    // Read query vector data
    SizeT query_vector_row_count{0};
    SizeT query_vector_dimension{0};
    float* query_vectors;
    {
        query_vectors = float_vector_read(sift1m_query, &query_vector_dimension, &query_vector_row_count, "Query Vector");
        assert(query_vector_dimension == dimension || !"query does not have same dimension as base set");
    }

    // Read ground truth
    SizeT top_k{0};                // number of results per query in the GT
    i32*  ground_truth{nullptr};   // number_of_queries * top_k matrix of ground-truth nearest-neighbors
    SizeT ground_truth_row_count{0};
    {
        // load ground-truth
        ground_truth = ivecs_read(sift1m_ground_truth, &top_k, &ground_truth_row_count);
        assert(query_vector_row_count == ground_truth_row_count || !"incorrect nb of ground truth entries");
    }

    infinity::BaseProfiler profiler;
    profiler.Begin();

//    single_run_on_cpu(index, query_vectors, query_vector_row_count, dimension, top_k, ground_truth);
//    batch_run_on_cpu(index, query_vectors, query_vector_row_count, dimension, top_k, ground_truth);
    scheduler_run_on_cpu(index, query_vectors, query_vector_row_count, dimension, top_k, ground_truth);
    profiler.End();
    std::cout << "Spend total: " << profiler.ElapsedToString() << std::endl;

    {
        delete[] query_vectors;
        delete[] ground_truth;
        delete index;
    }
    std::cout << "Memory: " << get_current_rss() / 1000000 << " MB" << std::endl;
}

void
benchmark_ivfflat(bool l2) {
    int d = 128;
    size_t centroids_count = 4096; //
    faiss::IndexFlatL2 quantizer(d);
    faiss::Index* index = nullptr;

    std::ifstream f(ivf_index_name);
    if(f.good()) {
        // Found index file
        std::cout << "Found index file ... " << std::endl;
        index = faiss::read_index(ivf_index_name);
    } else {
        {
            // Construct index
            infinity::BaseProfiler profiler;
            profiler.Begin();
            if(l2) {
                index = new faiss::IndexIVFFlat(&quantizer, d, centroids_count, faiss::MetricType::METRIC_L2);
            } else {
                index = new faiss::IndexIVFFlat(&quantizer, d, centroids_count, faiss::MetricType::METRIC_INNER_PRODUCT);
            }

            profiler.End();
            std::cout << "Create Flat index: " << profiler.ElapsedToString() << std::endl;
        }

        // No index file
        std::cout << "No index file found: constructing ... " << std::endl;
        {
            size_t nb, d2;
            infinity::BaseProfiler profiler;
            profiler.Begin();
            float* xb = fvecs_read(sift1m_train, &d2, &nb);
            profiler.End();
            std::cout << "Load sift1M learn data: " << profiler.ElapsedToString() << std::endl;

            profiler.Begin();
            index->train(nb, xb);
            profiler.End();
            std::cout << "Train sift1M learn data into quantizer: " << profiler.ElapsedToString() << std::endl;
            delete[] xb;
        }

        {
            size_t nb, d2;
            infinity::BaseProfiler profiler;
            profiler.Begin();
            float* xb = fvecs_read(sift1m_base, &d2, &nb);
            profiler.End();
            std::cout << "Load sift1M base data: " << profiler.ElapsedToString() << std::endl;

            profiler.Begin();
            index->add(nb, xb);
            profiler.End();
            std::cout << "Insert sift1M base data into index: " << profiler.ElapsedToString() << std::endl;
            delete[] xb;
        }
        faiss::write_index(index, ivf_index_name);
    }

    size_t number_of_queries;
    float* queries;
    {
        size_t d2;
        infinity::BaseProfiler profiler;
        profiler.Begin();
        queries = fvecs_read(sift1m_query, &d2, &number_of_queries);
        assert(d == d2 || !"query does not have same dimension as train set");
        profiler.End();
        std::cout << "Load sift1M query data: " << profiler.ElapsedToString() << std::endl;
    }

    size_t top_k;         // nb of results per query in the GT
    faiss::idx_t* ground_truth; // number_of_queries * top_k matrix of ground-truth nearest-neighbors
    {
        // load ground-truth and convert int to long
        size_t nq2;
        infinity::BaseProfiler profiler;
        profiler.Begin();

        int* gt_int = ivecs_read(sift1m_ground_truth, &top_k, &nq2);
        assert(nq2 == number_of_queries || !"incorrect nb of ground truth entries");

        ground_truth = new faiss::idx_t[top_k * number_of_queries];
        for (int i = 0; i < top_k * number_of_queries; ++ i) {
            ground_truth[i] = gt_int[i];
        }
        delete[] gt_int;
        profiler.End();
        std::cout << "Load sift1M ground truth and load row id: " << profiler.ElapsedToString() << std::endl;
    }

    {
        infinity::BaseProfiler profiler;
        profiler.Begin();
        faiss::idx_t* I = new faiss::idx_t[number_of_queries * top_k];
        float* D = new float[number_of_queries * top_k];

        faiss::IndexIVFFlat* ivf_flat_index = (faiss::IndexIVFFlat*)index;

        ivf_flat_index->nprobe = 4; // Default value is 1;
        ivf_flat_index->search(number_of_queries, queries, top_k, D, I);
        profiler.End();
        std::cout << "Search: " << profiler.ElapsedToString() << std::endl;

        // evaluate result by hand.
        int n_1 = 0, n_10 = 0, n_100 = 0;
        for (int i = 0; i < number_of_queries; i++) {
            int gt_nn = ground_truth[i * top_k];
            for (int j = 0; j < top_k; j++) {
                if (I[i * top_k + j] == gt_nn) {
                    if (j < 1)
                        n_1++;
                    if (j < 10)
                        n_10++;
                    if (j < 100)
                        n_100++;
                }
            }
        }
        printf("R@1 = %.4f\n", n_1 / float(number_of_queries));
        printf("R@10 = %.4f\n", n_10 / float(number_of_queries));
        printf("R@100 = %.4f\n", n_100 / float(number_of_queries));

        delete[] I;
        delete[] D;
    }

    {
        delete index;
        delete[] ground_truth;
        delete[] queries;
    }
}

void
scheduler_test() {
    Scheduler scheduler;
    u32 thread_count = std::thread::hardware_concurrency();
    Vector<UniquePtr<DummyTask>> tasks;
    tasks.reserve(thread_count);
    HashSet<i64> cpu_set;
    for(i64 idx = 0; idx < thread_count; ++ idx) {
        cpu_set.insert(idx);
        tasks.emplace_back(std::move(MakeUnique<DummyTask>()));
    }

    scheduler.Init(cpu_set);
    for(i64 idx = 0; idx < 5; ++ idx) {
        for(i64 cpu_id: cpu_set) {
            scheduler.ScheduleTask(cpu_id, tasks[cpu_id].get());
        }
    }

    scheduler.Uninit();
}

auto main () -> int {
    benchmark_flat();
//    benchmark_ivfflat(true);
//    scheduler_test();
    return 0;
}