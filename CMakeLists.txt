# (C) 2022 by folkert van heusden <mail@vanheusden.com>, released under Apache License v2.0
cmake_minimum_required(VERSION 3.9.4)

project(mystorage VERSION 6.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_definitions("-D_FILE_OFFSET_BITS=64")

set(CMAKE_BUILD_TYPE Debug)

add_compile_options(-Wall -pedantic -Wgnu-zero-variadic-macro-arguments)

add_executable(mystorage
	block.cpp
	compresser.cpp
	compresser_lzo.cpp
	compresser_zlib.cpp
	error.cpp
	io.cpp
	lock_group.cpp
	logging.cpp
	main.cpp
	nbd.cpp
	net.cpp
	socket_listener.cpp
	socket_listener_ipv4.cpp
	storage_backend.cpp
	storage_backend_compressed_dir.cpp
	storage_backend_file.cpp
	str.cpp
	time.cpp
	)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads)
target_link_libraries(mystorage Threads::Threads)

include(FindPkgConfig)

pkg_check_modules(ZLIB REQUIRED zlib)
target_link_libraries(mystorage ${ZLIB_LIBRARIES})
target_include_directories(mystorage PUBLIC ${ZLIB_INCLUDE_DIRS})
target_compile_options(mystorage PUBLIC ${ZLIB_CFLAGS_OTHER})

pkg_check_modules(LZO2 REQUIRED lzo2)
target_link_libraries(mystorage ${LZO2_LIBRARIES})
target_include_directories(mystorage PUBLIC ${LZO2_INCLUDE_DIRS})
target_compile_options(mystorage PUBLIC ${LZO2_CFLAGS_OTHER})
