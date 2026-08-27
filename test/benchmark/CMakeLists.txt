add_executable(rpc_benchmark rpc_benchmark.cc ../../example/checkOrder.pb.cc)
target_link_libraries(rpc_benchmark mprpc protobuf pthread)
