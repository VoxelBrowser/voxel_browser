# Engine modules + executables. One add_subdirectory per module (spec §4).

add_subdirectory(core)
add_subdirectory(auth)
add_subdirectory(ecs)
add_subdirectory(protocol)
add_subdirectory(world)
add_subdirectory(worldgen)
add_subdirectory(physics)
add_subdirectory(script)
add_subdirectory(assetsync)
add_subdirectory(replication)
add_subdirectory(net)

if(VB_WITH_AUTOMATION)
  add_subdirectory(automation)
endif()

if(VB_BUILD_CLIENT)
  add_subdirectory(render)
endif()

add_subdirectory(editor)

if(VB_BUILD_SERVER)
  add_subdirectory(server)
  add_subdirectory(loadtest)
endif()

if(VB_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(VB_BUILD_CLIENT)
  add_subdirectory(client)
endif()
