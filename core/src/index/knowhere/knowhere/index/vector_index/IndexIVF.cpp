// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include <faiss/AutoTune.h>
#include <faiss/IVFlib.h>
#include <faiss/IndexFlat.h>
#include <faiss/IndexIVF.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/clone_index.h>
#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#ifdef MILVUS_GPU_VERSION
#include <faiss/gpu/GpuAutoTune.h>
#include <faiss/gpu/GpuCloner.h>
#endif

#include <fiu-local.h>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "knowhere/adapter/VectorAdapter.h"
#include "knowhere/common/Exception.h"
#include "knowhere/common/Log.h"
#ifdef MILVUS_GPU_VERSION
#include "knowhere/index/vector_index/IndexGPUIVF.h"
#endif
#include "knowhere/index/vector_index/IndexIVF.h"

namespace knowhere {

using stdclock = std::chrono::high_resolution_clock;

IndexModelPtr
IVF::Train(const DatasetPtr& dataset, const Config& config) {
    GETTENSOR(dataset)

    faiss::Index* coarse_quantizer = new faiss::IndexFlatL2(dim);
    auto index = std::make_shared<faiss::IndexIVFFlat>(coarse_quantizer, dim, config[IndexParams::nlist].get<int64_t>(),
                                                       GetMetricType(config[Metric::TYPE].get<std::string>()));
    index->train(rows, (float*)p_data);

    // TODO(linxj): override here. train return model or not.
    return std::make_shared<IVFIndexModel>(index);
}

void
IVF::Add(const DatasetPtr& dataset, const Config& config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    std::lock_guard<std::mutex> lk(mutex_);
    GETTENSOR(dataset)

    auto p_ids = dataset->Get<const int64_t*>(meta::IDS);
    index_->add_with_ids(rows, (float*)p_data, p_ids);
}

void
IVF::AddWithoutIds(const DatasetPtr& dataset, const Config& config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    std::lock_guard<std::mutex> lk(mutex_);
    GETTENSOR(dataset)

    index_->add(rows, (float*)p_data);
}

BinarySet
IVF::Serialize() {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    std::lock_guard<std::mutex> lk(mutex_);
    return SerializeImpl();
}

void
IVF::Load(const BinarySet& index_binary) {
    std::lock_guard<std::mutex> lk(mutex_);
    LoadImpl(index_binary);
}

DatasetPtr
IVF::Search(const DatasetPtr& dataset, const Config& config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    GETTENSOR(dataset)

    try {
        fiu_do_on("IVF.Search.throw_std_exception", throw std::exception());
        fiu_do_on("IVF.Search.throw_faiss_exception", throw faiss::FaissException(""));
        auto elems = rows * config[meta::TOPK].get<int64_t>();

        size_t p_id_size = sizeof(int64_t) * elems;
        size_t p_dist_size = sizeof(float) * elems;
        auto p_id = (int64_t*)malloc(p_id_size);
        auto p_dist = (float*)malloc(p_dist_size);

        search_impl(rows, (float*)p_data, config[meta::TOPK].get<int64_t>(), p_dist, p_id, config);

        //    std::stringstream ss_res_id, ss_res_dist;
        //    for (int i = 0; i < 10; ++i) {
        //        printf("%llu", res_ids[i]);
        //        printf("\n");
        //        printf("%.6f", res_dis[i]);
        //        printf("\n");
        //        ss_res_id << res_ids[i] << " ";
        //        ss_res_dist << res_dis[i] << " ";
        //    }
        //    std::cout << std::endl << "after search: " << std::endl;
        //    std::cout << ss_res_id.str() << std::endl;
        //    std::cout << ss_res_dist.str() << std::endl << std::endl;

        auto ret_ds = std::make_shared<Dataset>();
        ret_ds->Set(meta::IDS, p_id);
        ret_ds->Set(meta::DISTANCE, p_dist);
        return ret_ds;
    } catch (faiss::FaissException& e) {
        KNOWHERE_THROW_MSG(e.what());
    } catch (std::exception& e) {
        KNOWHERE_THROW_MSG(e.what());
    }
}

void
IVF::set_index_model(IndexModelPtr model) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto rel_model = std::static_pointer_cast<IVFIndexModel>(model);

    // Deep copy here.
    index_.reset(faiss::clone_index(rel_model->index_.get()));
}

std::shared_ptr<faiss::IVFSearchParameters>
IVF::GenParams(const Config& config) {
    auto params = std::make_shared<faiss::IVFSearchParameters>();

    params->nprobe = config[IndexParams::nprobe];
    // params->max_codes = config["max_codes"];

    return params;
}

int64_t
IVF::Count() {
    return index_->ntotal;
}

int64_t
IVF::Dimension() {
    return index_->d;
}

void
IVF::GenGraph(const float* data, const int64_t& k, Graph& graph, const Config& config) {
    int64_t K = k + 1;
    auto ntotal = Count();

    size_t dim = config[meta::DIM];
    auto batch_size = 1000;
    auto tail_batch_size = ntotal % batch_size;
    auto batch_search_count = ntotal / batch_size;
    auto total_search_count = tail_batch_size == 0 ? batch_search_count : batch_search_count + 1;

    std::vector<float> res_dis(K * batch_size);
    graph.resize(ntotal);
    Graph res_vec(total_search_count);
    for (int i = 0; i < total_search_count; ++i) {
        auto b_size = (i == (total_search_count - 1)) && tail_batch_size != 0 ? tail_batch_size : batch_size;

        auto& res = res_vec[i];
        res.resize(K * b_size);

        auto xq = data + batch_size * dim * i;
        search_impl(b_size, (float*)xq, K, res_dis.data(), res.data(), config);

        for (int j = 0; j < b_size; ++j) {
            auto& node = graph[batch_size * i + j];
            node.resize(k);
            auto start_pos = j * K + 1;
            for (int m = 0, cursor = start_pos; m < k && cursor < start_pos + k; ++m, ++cursor) {
                node[m] = res[cursor];
            }
        }
    }
}

void
IVF::search_impl(int64_t n, const float* data, int64_t k, float* distances, int64_t* labels, const Config& cfg) {
    auto params = GenParams(cfg);
    auto ivf_index = dynamic_cast<faiss::IndexIVF*>(index_.get());
    ivf_index->nprobe = params->nprobe;
    stdclock::time_point before = stdclock::now();
    ivf_index->search(n, (float*)data, k, distances, labels, bitset_);
    stdclock::time_point after = stdclock::now();
    double search_cost = (std::chrono::duration<double, std::micro>(after - before)).count();
    KNOWHERE_LOG_DEBUG << "IVF search cost: " << search_cost
                       << ", quantization cost: " << faiss::indexIVF_stats.quantization_time
                       << ", data search cost: " << faiss::indexIVF_stats.search_time;
    faiss::indexIVF_stats.quantization_time = 0;
    faiss::indexIVF_stats.search_time = 0;
}

VectorIndexPtr
IVF::CopyCpuToGpu(const int64_t& device_id, const Config& config) {
#ifdef MILVUS_GPU_VERSION

    if (auto res = FaissGpuResourceMgr::GetInstance().GetRes(device_id)) {
        ResScope rs(res, device_id, false);
        auto gpu_index = faiss::gpu::index_cpu_to_gpu(res->faiss_res.get(), device_id, index_.get());

        std::shared_ptr<faiss::Index> device_index;
        device_index.reset(gpu_index);
        return std::make_shared<GPUIVF>(device_index, device_id, res);
    } else {
        KNOWHERE_THROW_MSG("CopyCpuToGpu Error, can't get gpu_resource");
    }

#else
    KNOWHERE_THROW_MSG("Calling IVF::CopyCpuToGpu when we are using CPU version");
#endif
}

// VectorIndexPtr
// IVF::Clone() {
//    std::lock_guard<std::mutex> lk(mutex_);
//
//    auto clone_index = faiss::clone_index(index_.get());
//    std::shared_ptr<faiss::Index> new_index;
//    new_index.reset(clone_index);
//    return Clone_impl(new_index);
//}
//
// VectorIndexPtr
// IVF::Clone_impl(const std::shared_ptr<faiss::Index>& index) {
//    return std::make_shared<IVF>(index);
//}

void
IVF::Seal() {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }
    SealImpl();
}

DatasetPtr
IVF::GetVectorById(const DatasetPtr& dataset, const Config& config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    auto p_data = dataset->Get<const int64_t*>(meta::IDS);
    auto elems = dataset->Get<int64_t>(meta::DIM);

    try {
        size_t p_x_size = sizeof(float) * elems;
        auto p_x = (float*)malloc(p_x_size);

        auto index_ivf = std::static_pointer_cast<faiss::IndexIVF>(index_);
        index_ivf->get_vector_by_id(1, p_data, p_x, bitset_);

        auto ret_ds = std::make_shared<Dataset>();
        ret_ds->Set(meta::TENSOR, p_x);
        return ret_ds;
    } catch (faiss::FaissException& e) {
        KNOWHERE_THROW_MSG(e.what());
    } catch (std::exception& e) {
        KNOWHERE_THROW_MSG(e.what());
    }
}

DatasetPtr
IVF::SearchById(const DatasetPtr& dataset, const Config& config) {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("index not initialize or trained");
    }

    auto rows = dataset->Get<int64_t>(meta::ROWS);
    auto p_data = dataset->Get<const int64_t*>(meta::IDS);

    try {
        auto elems = rows * config[meta::TOPK].get<int64_t>();

        size_t p_id_size = sizeof(int64_t) * elems;
        size_t p_dist_size = sizeof(float) * elems;
        auto p_id = (int64_t*)malloc(p_id_size);
        auto p_dist = (float*)malloc(p_dist_size);

        // todo: enable search by id (zhiru)
        //        auto blacklist = dataset->Get<faiss::ConcurrentBitsetPtr>("bitset");
        auto index_ivf = std::static_pointer_cast<faiss::IndexIVF>(index_);
        index_ivf->search_by_id(rows, p_data, config[meta::TOPK].get<int64_t>(), p_dist, p_id, bitset_);

        //    std::stringstream ss_res_id, ss_res_dist;
        //    for (int i = 0; i < 10; ++i) {
        //        printf("%llu", res_ids[i]);
        //        printf("\n");
        //        printf("%.6f", res_dis[i]);
        //        printf("\n");
        //        ss_res_id << res_ids[i] << " ";
        //        ss_res_dist << res_dis[i] << " ";
        //    }
        //    std::cout << std::endl << "after search: " << std::endl;
        //    std::cout << ss_res_id.str() << std::endl;
        //    std::cout << ss_res_dist.str() << std::endl << std::endl;

        auto ret_ds = std::make_shared<Dataset>();
        ret_ds->Set(meta::IDS, p_id);
        ret_ds->Set(meta::DISTANCE, p_dist);
        return ret_ds;
    } catch (faiss::FaissException& e) {
        KNOWHERE_THROW_MSG(e.what());
    } catch (std::exception& e) {
        KNOWHERE_THROW_MSG(e.what());
    }
}

void
IVF::SetBlacklist(faiss::ConcurrentBitsetPtr list) {
    bitset_ = std::move(list);
}

void
IVF::GetBlacklist(faiss::ConcurrentBitsetPtr& list) {
    list = bitset_;
}

IVFIndexModel::IVFIndexModel(std::shared_ptr<faiss::Index> index) : FaissBaseIndex(std::move(index)) {
}

BinarySet
IVFIndexModel::Serialize() {
    if (!index_ || !index_->is_trained) {
        KNOWHERE_THROW_MSG("indexmodel not initialize or trained");
    }
    std::lock_guard<std::mutex> lk(mutex_);
    return SerializeImpl();
}

void
IVFIndexModel::Load(const BinarySet& binary_set) {
    std::lock_guard<std::mutex> lk(mutex_);
    LoadImpl(binary_set);
}

void
IVFIndexModel::SealImpl() {
    // do nothing
}

}  // namespace knowhere
