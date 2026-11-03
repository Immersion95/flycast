target_sources(${PROJECT_NAME} PRIVATE
        directory.cpp
        directory.h
        host_context.h
        i18n.cpp
        i18n.h
        oslib.h
        precise_sleep.cpp
        precise_sleep.h
        resources.cpp
        resources.h
        storage.cpp
        storage.h
        unwind_info.h
        virtmem.h)

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
            http_client.cpp
            http_client.h
            oslib.cpp)
endif()
