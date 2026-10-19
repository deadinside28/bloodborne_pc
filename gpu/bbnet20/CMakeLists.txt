# bbport online module, second generation: libbbnet20.so. shadPS4 v0.20.0's network, NP and
# shadNet sources (vendor/shadps4, 529ceb3, with vendor-changes.diff) and The Hunter's Dream
# backend (thd/, from droogie/bbhost), as Supermedo/Awaken put them together
# (SHADNET-PORT.md). Loaded instead of libbbnet.so with BB_NET_MODULE (the launcher's pools
# "shadnet" and "huntersdream"); the shadp2p module (../bbnet, BBSC) is untouched. Hidden
# symbols and -Bsymbolic as there: the two modules never meet each other or libbbgpu.

set(BBPORT_VERSION "0.6-pre1")
set(BBNET_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/shadps4")
set(SHADNET_PROTO_OUT20 "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT20}")
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT20}/shadnet.pb.cc" "${SHADNET_PROTO_OUT20}/shadnet.pb.h"
    COMMAND protobuf::protoc "--proto_path=${BBNET_VENDOR}/shadnet"
            "--cpp_out=${SHADNET_PROTO_OUT20}" "${BBNET_VENDOR}/shadnet/shadnet.proto"
    DEPENDS "${BBNET_VENDOR}/shadnet/shadnet.proto")
file(GLOB_RECURSE BBNET20_SOURCES CONFIGURE_DEPENDS
    vendor/shadps4/core/libraries/net/*.cpp vendor/shadps4/core/libraries/network/*.cpp
    vendor/shadps4/core/libraries/np/*.cpp vendor/shadps4/core/net/*.cpp
    vendor/shadps4/shadnet/*.cpp)
# Trophies, commerce and the profile and Facebook dialogs stay in the C runtime.
list(FILTER BBNET20_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")
# BBSC, Bloodborne seamless co-op (GPL-2.0+): its game code and the port's stand-ins for
# shadPS4's UI (bbsc/bbsc_glue.cpp). Installed only for Bloodborne with BB_BBSC=1.
file(GLOB BBNET20_BBSC_SOURCES CONFIGURE_DEPENDS bbsc/core/*.cpp bbsc/bbsc_glue.cpp)
file(GLOB BBNET20_THD_SOURCES CONFIGURE_DEPENDS thd/*.cpp compat/common/*.cpp)

add_library(bbnet20 SHARED ${BBNET20_SOURCES} ${BBNET20_THD_SOURCES} ${BBNET20_BBSC_SOURCES}
    "${SHADNET_PROTO_OUT20}/shadnet.pb.cc" bbnet_glue.cpp)
target_include_directories(bbnet20 PRIVATE include bbsc compat . .. ../shim "${BBNET_VENDOR}"
    "${BBNET_VENDOR}/shadnet" "${SHADNET_PROTO_OUT20}" ../third_party/imgui ../third_party/httplib)
# The version the shadNet client reports to the server (BB_CLIENT_VERSION overrides it).
target_compile_definitions(bbnet20 PRIVATE BOOST_ASIO_STANDALONE NDEBUG
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT BBNET_PORT_VERSION="${BBPORT_VERSION}")
target_compile_options(bbnet20 PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden
    -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_options(bbnet20 PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
set_target_properties(bbnet20 PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(bbnet20 PRIVATE bbgpu nlohmann_json::nlohmann_json protobuf::libprotobuf
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB "${MINIUPNPC_LIBRARY}" Vulkan::Headers fmt::fmt
    Boost::headers magic_enum::magic_enum)
target_include_directories(bbnet20 PRIVATE "${MINIUPNPC_INCLUDE}")
