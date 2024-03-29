# Enable c99 support.
use_c99()

include_directories(${sos_flow_SOURCE_DIR}/src)

# If using SQLite3, add include files - it's required
if(SQLite3_FOUND)
    include_directories(${SQLite3_INCLUDE_DIR})
    link_libraries(${SQLite3_LIBRARY})
endif(SQLite3_FOUND)

# If using MPI, add the include files
if(MPI_FOUND)
	include_directories(${MPI_INCLUDE_PATH})
    link_libraries(${MPI_CXX_LIBRARIES})
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${MPI_C_COMPILE_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MPI_CXX_COMPILE_FLAGS}")
    # We need C++ because of sosd_spawn.cpp
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MPI_CXX_LINK_FLAGS}")
endif(MPI_FOUND)

if(EVPath_FOUND)
    include_directories(${EVPath_INCLUDE_DIR})
    link_libraries(${EVPath_LIBRARIES})
endif(EVPath_FOUND)

if(ZeroMQ_FOUND)
    include_directories(${ZeroMQ_INCLUDE_DIR})
    link_libraries(${ZeroMQ_LIBRARIES})
endif(ZeroMQ_FOUND)

if(CZMQ_FOUND)
    include_directories(${CZMQ_INCLUDE_DIR})
    link_libraries(${CZMQ_LIBRARIES})
    message(STATUS "NOTE: CZMQ_INCLUDE_DIR=${CZMQ_INCLUDE_DIR}")
endif(CZMQ_FOUND)

if(Munge_FOUND)
    include_directories(${Munge_INCLUDE_DIR})
    link_libraries(${Munge_LIBRARIES})
endif(Munge_FOUND)

if(SOSD_DAEMON_DETACH)
    message(STATUS "DAEMON_DETACH:  Daemon is set to DETACH and run in the background...")
    add_definitions("-DSOSD_DAEMON_MODE=1")
else()
    message(STATUS "DAEMON_DETACH:  Daemon is set to run INTERACTIVELY...")
    add_definitions("-DSOSD_DAEMON_MODE=0")
endif()

#TARGET ---> sos (library)
add_library(sos
    sos.c
    ssos.c
    sosa.c
    sos_buffer.c
    sos_string.c
    sos_qhashtbl.c
    sos_pipe.c
    sos_target.c
    sos_re.c
    sos_error.c
    sos_options.c)
#----
target_link_libraries(sos ${LIBS} dl m)

file(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake)
export(TARGETS sos APPEND FILE ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/sosflow.cmake)

#TARGET ---> sosd (daemon)
if(SOS_CLOUD_SYNC)
    add_definitions("-DSOSD_CLOUD_SYNC")

    if(SOS_CLOUD_SYNC_WITH_MPI)
        message(STATUS "CLOUD_SYNC:  Configuring daemon to use MPI.")
        add_executable(sosd sos.c sosd.c sosd_cloud_mpi.c sosd_db_sqlite.c sosd_system.cpp)
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_DAEMON_SRC")
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_CLOUD_SYNC_WITH_MPI")
        target_link_libraries(sosd sos stdc++)
        if(NOT APPLE)
            # Apple does not support the '-lrt' compiler option.
            target_link_libraries("sosd" "rt")
        endif()
    elseif(SOS_CLOUD_SYNC_WITH_EVPATH)
        message(STATUS "CLOUD_SYNC:  Configuring daemon to use EVPath.")
        add_executable(sosd sos.c sosd.c sosd_cloud_evpath.c sosd_db_sqlite.c sosd_system.cpp)
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_DAEMON_SRC")
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_CLOUD_SYNC_WITH_EVPATH")
        target_link_libraries(sosd sos stdc++)
        if(NOT APPLE)
            # Apple does no support the '-lrt' compiler option.
            target_link_libraries("sosd" "rt")
        endif()
    elseif(SOS_CLOUD_SYNC_WITH_SOCKET)
        message(STATUS "CLOUD_SYNC:  Configuring daemon to use socket (TCP/IP).")
        add_executable(sosd sos.c sosd.c sos_target.c sosd_cloud_socket.c sosd_db_sqlite.c sosd_system.cpp)
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_DAEMON_SRC")
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_CLOUD_SYNC_WITH_SOCKET")
        target_link_libraries(sosd sos stdc++)
        if(NOT APPLE)
            # Apple does no support the '-lrt' compiler option.
            target_link_libraries("sosd" "rt")
        endif()
    elseif(SOS_CLOUD_SYNC_WITH_ZEROMQ)
        message(STATUS "CLOUD_SYNC:  Configuring daemon to use ZeroMQ.")
        add_executable(sosd sos.c sosd.c sosd_cloud_zeromq.c sosd_db_sqlite.c sosd_system.cpp)
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_DAEMON_SRC")
        set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_CLOUD_SYNC_WITH_ZEROMQ")
        target_link_libraries(sosd sos stdc++)
        if(NOT APPLE)
            # Apple does no support the '-lrt' compiler option.
            target_link_libraries("sosd" "rt")
        endif()
    else()
        message(FATAL_ERROR "You have specified that cloud_sync is activated, but not specified a supported method.")
    endif()
else()
    message(STATUS "CLOUD_SYNC:  Building daemon with no support for synchonization.")
    message(STATUS "CLOUD_SYNC:  This is NOT RECOMMENDED.")
    add_executable(sosd sos.c sosd.c sosd_cloud_stubs.c sosd_db_sqlite.c sosd_system.cpp)
    set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_DAEMON_SRC")
    set_target_properties(sosd PROPERTIES COMPILE_FLAGS "-DSOSD_CLOUD_SYNC_WITH_STUBS")
    target_link_libraries(sosd sos stdc++ dl)
    if(NOT APPLE)
        # Apple does no support the '-lrt' compiler option.
        target_link_libraries(sosd rt)
    endif()
endif()

#TARGET ---> sosd_stop (shut down the daemon)
add_executable(sosd_stop sosd_stop.c)
target_link_libraries(sosd_stop sos)

#TARGET ---> sosd_trigger (trigger feedback from aggregator to listeners)
add_executable(sosd_trigger sosd_trigger.c)
target_link_libraries(sosd_trigger sos)

#TARGET ---> sosd_probe (get daemon satus and display it)
add_executable(sosd_probe sosd_probe.c)
target_link_libraries(sosd_probe sos)

#TARGET ---> sosd_manifest (list of pubs this daemon is aware of, w/latest frame #)
add_executable(sosd_manifest sosd_manifest.c)
target_link_libraries(sosd_manifest sos)

# sosd_spawn is used to spawn a listener if one isn't running
if(MPI_FOUND)
    SET(MPI_SOURCE_FILES "sosd_spawn.cpp")
else(MPI_FOUND)
    SET(MPI_SOURCE_FILES "sosd_spawn_nompi.cpp")
endif(MPI_FOUND)

#TARGET ---> demo_app (demonstration / example)
add_executable(demo_app ${MPI_SOURCE_FILES} demo_app.c)
target_link_libraries(demo_app sos)

#TARGET ---> tau_test (demonstration / example / unit test)
add_executable(tau_test ${MPI_SOURCE_FILES} tau_test.c)
target_link_libraries(tau_test sos)

#TARGET ---> proc_app (demonstration / example)
add_executable(proc_app proc_app.c)
target_link_libraries(proc_app sos)

INSTALL(FILES ${sos_flow_SOURCE_DIR}/scripts/showdb
              ${sos_flow_SOURCE_DIR}/scripts/evp.start.2
              ${sos_flow_SOURCE_DIR}/scripts/evp.kill.2
              ${sos_flow_SOURCE_DIR}/scripts/evp.cleanall
              ${sos_flow_SOURCE_DIR}/scripts/wipedb
              DESTINATION bin
              PERMISSIONS OWNER_READ OWNER_EXECUTE OWNER_WRITE
                          GROUP_READ GROUP_EXECUTE
                          WORLD_READ WORLD_EXECUTE)
INSTALL(FILES sos.h
              ssos.h
              sosa.h
              sos_types.h
              sos_qhashtbl.h
              sos_pipe.h
              sos_buffer.h
              sos_string.h
              sos_target.h
              sos_re.h
              DESTINATION include)
INSTALL(TARGETS
    sos
    sosd
    sosd_stop
    sosd_probe
    sosd_manifest
    sosd_trigger
    proc_app
    demo_app
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)


