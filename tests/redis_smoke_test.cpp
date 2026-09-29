#include <sw/redis++/redis++.h>
#include <iostream>

int main(){
    sw::redis::Redis redis("tcp://127.0.0.1:6379");

    redis.set("smoke_test_key", "hello_from_cpp");
    auto value = redis.get("smoke_test_key");

    if (value) {
        std::cout << "Got value: " << *value << std::endl;
    } else {
        std::cout << "Key not found." << std::endl;
    }

    return 0;
}