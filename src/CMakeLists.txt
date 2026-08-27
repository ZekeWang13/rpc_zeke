#aux_source_directory(. SRC_LIST)
set(SRC_LIST 
    mprpcapplication.cc 
    mprpcconfig.cc 
    rpcheader.pb.cc 
    rpcprovider.cc 
    mprpcchannel.cc
    mprpccontroller.cc
    logger.cc
    zookeeperutil.cc
    circuitbreaker.cc
    servicediscovery.cc)
add_library(mprpc ${SRC_LIST})
target_link_libraries(mprpc muduo_net muduo_base pthread zookeeper_mt)
if(MPRPC_ENABLE_REQUEST_TRACE)
    target_compile_definitions(mprpc PRIVATE MPRPC_ENABLE_REQUEST_TRACE=1)
endif()
