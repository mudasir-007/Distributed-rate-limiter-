#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sharded_rate_limiter.h>

int main(){
    const double capacity = 10;
    ratelimiter::ShardedRateLimiter limiter(capacity, /*refill_rate_per_sec=*/0.0,
                                           /*num_shards=*/16);
    
    const int num_keys = 20;
    const int threads_per_key = 4;
    const int attempts_per_thread = 10;
    
    std::vector<std::thread> threads;
    std::vector<std::atomic<int>> success_counts(num_keys);
    for(auto& c: success_counts){
        c.store(0);
    }

    for (int k=0; k < num_keys; ++k){
        std::string key = "user_" + std::to_string(k);
        for (int t=0; t < threads_per_key; ++t){
            threads.emplace_back([&limiter, key, &success_counts, k, attempts_per_thread](){
                for (int i=0; i<attempts_per_thread;++i){
                    if (limiter.tryAcquire(key)){
                        success_counts[k].fetch_add(1,std::memory_order_relaxed);
                    }
                }
            });
        }
    }

    for (auto& th: threads){
        th.join();
    }

    bool all_correct = true;
    for (int k=0; k<num_keys; ++k){
        int count = success_counts[k].load();
        std::cout<<"user_"<<k<<": "<<count<<" grants (expected "<< capacity <<")\n";
        if (count != static_cast<int>(capacity)){
            all_correct = false;
        }
    }

    if (all_correct) {
        std::cout << "\nPASS: every key independently received exactly its capacity.\n";
        return 0;
    } else {
        std::cout << "\nFAIL: at least one key's count was wrong.\n";
    }
    return 1;
}
