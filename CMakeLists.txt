# (C) 2022 by folkert van heusden <mail@vanheusden.com>, released under Apache License v2.0
cmake_minimum_required(VERSION 3.9.4)

project(mystorage VERSION 6.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_definitions("-D_FILE_OFFSET_BITS=64")

set(CMAKE_BUILD_TYPE Debug)

add_compile_options(-Wall -pedantic)

add_executable(mystorage
	aoe-common.cpp
	base.cpp
	block.cpp
	compresser.cpp
	compresser_lzo.cpp
	compresser_zlib.cpp
	error.cpp
	hash.cpp
	hash_sha384.cpp
	io.cpp
	journal.cpp
	lock_group.cpp
	logging.cpp
	main.cpp
	mirror.cpp
	mirror_storage_backend.cpp
	net.cpp
	server.cpp
	server_aoe.cpp
	server_nbd.cpp
	snapshots.cpp
	socket_client.cpp
	socket_client_ipv4.cpp
	socket_client_ipv6.cpp
	socket_listener.cpp
	socket_listener_ipv4.cpp
	socket_listener_ipv6.cpp
	socket_listener_unixdomain.cpp
	storage_backend.cpp
	storage_backend_aoe.cpp
	storage_backend_compressed_dir.cpp
	storage_backend_file.cpp
	storage_backend_dedup.cpp
	storage_backend_nbd.cpp
	str.cpp
	tiering.cpp
	time.cpp
	yaml-helpers.cpp
	)

add_executable(test-mystorage
	aoe-common.cpp
	base.cpp
	block.cpp
	compresser.cpp
	compresser_lzo.cpp
	compresser_zlib.cpp
	error.cpp
	hash.cpp
	hash_sha384.cpp
	io.cpp
	journal.cpp
	lock_group.cpp
	logging.cpp
	test.cpp
	mirror.cpp
	mirror_storage_backend.cpp
	net.cpp
	server.cpp
	server_aoe.cpp
	server_nbd.cpp
	snapshots.cpp
	socket_client.cpp
	socket_client_ipv4.cpp
	socket_client_ipv6.cpp
	socket_listener.cpp
	socket_listener_ipv4.cpp
	socket_listener_ipv6.cpp
	socket_listener_unixdomain.cpp
	storage_backend.cpp
	storage_backend_aoe.cpp
	storage_backend_compressed_dir.cpp
	storage_backend_file.cpp
	storage_backend_dedup.cpp
	storage_backend_nbd.cpp
	str.cpp
	tiering.cpp
	time.cpp
	yaml-helpers.cpp
	)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads)
target_link_libraries(mystorage Threads::Threads)
target_link_libraries(test-mystorage Threads::Threads)

include(FindPkgConfig)

pkg_check_modules(ZLIB REQUIRED zlib)
target_link_libraries(mystorage ${ZLIB_LIBRARIES})
target_include_directories(mystorage PUBLIC ${ZLIB_INCLUDE_DIRS})
target_compile_options(mystorage PUBLIC ${ZLIB_CFLAGS_OTHER})
target_link_libraries(test-mystorage ${ZLIB_LIBRARIES})
target_include_directories(test-mystorage PUBLIC ${ZLIB_INCLUDE_DIRS})
target_compile_options(test-mystorage PUBLIC ${ZLIB_CFLAGS_OTHER})

pkg_check_modules(LZO2 REQUIRED lzo2)
target_link_libraries(mystorage ${LZO2_LIBRARIES})
target_include_directories(mystorage PUBLIC ${LZO2_INCLUDE_DIRS})
target_compile_options(mystorage PUBLIC ${LZO2_CFLAGS_OTHER})
target_link_libraries(test-mystorage ${LZO2_LIBRARIES})
target_include_directories(test-mystorage PUBLIC ${LZO2_INCLUDE_DIRS})
target_compile_options(test-mystorage PUBLIC ${LZO2_CFLAGS_OTHER})

pkg_check_modules(CRYPTOPP REQUIRED libcrypto++)
target_link_libraries(mystorage ${CRYPTOPP_LIBRARIES})
target_include_directories(mystorage PUBLIC ${CRYPTOPP_INCLUDE_DIRS})
target_compile_options(mystorage PUBLIC ${CRYPTOPP_CFLAGS_OTHER})
target_link_libraries(test-mystorage ${CRYPTOPP_LIBRARIES})
target_include_directories(test-mystorage PUBLIC ${CRYPTOPP_INCLUDE_DIRS})
target_compile_options(test-mystorage PUBLIC ${CRYPTOPP_CFLAGS_OTHER})

pkg_check_modules(KC REQUIRED kyotocabinet)
target_link_libraries(mystorage ${KC_LIBRARIES})
target_include_directories(mystorage PUBLIC ${KC_INCLUDE_DIRS})
target_compile_options(mystorage PUBLIC ${KC_CFLAGS_OTHER})
target_link_libraries(test-mystorage ${KC_LIBRARIES})
target_include_directories(test-mystorage PUBLIC ${KC_INCLUDE_DIRS})
target_compile_options(test-mystorage PUBLIC ${KC_CFLAGS_OTHER})

pkg_check_modules(YAML REQUIRED yaml-cpp)
target_link_libraries(mystorage ${YAML_LIBRARIES})
target_include_directories(mystorage PUBLIC ${YAML_INCLUDE_DIRS})
target_compile_options(mystorage PUBLIC ${YAML_CFLAGS_OTHER})
target_link_libraries(test-mystorage ${YAML_LIBRARIES})
target_include_directories(test-mystorage PUBLIC ${YAML_INCLUDE_DIRS})
target_compile_options(test-mystorage PUBLIC ${YAML_CFLAGS_OTHER})
