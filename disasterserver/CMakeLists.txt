option(DYLIB "Builds dynamic library" OFF)
option(BUILD_UI "Builds UI using SDL" OFF)


include(TestBigEndian)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)

if(IS_BIG_ENDIAN)
	add_compile_definitions(SYS_BIG_ENDIAN)
endif()

if(DYLIB)
	add_compile_definitions(SYS_DYLIB)
endif()

if(BUILD_UI)
	add_compile_definitions(SYS_USE_SDL2)
endif()

if(ANDROID)
	add_compile_definitions(SYS_ANDROID)
endif()

if((CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "GNU"))
	set(CMAKE_C_FLAGS_DEBUG "-g")
	set(CMAKE_C_FLAGS_RELEASE "-O3")
	message("Clang/GCC flags applied")
endif()

set(SOURCES
	"Auth.c"
	"Mod.c"
	"modobj.c"
	"Config.c"
	"cJSON.c"
	"UTF8.c"
	"CMath.c"
	"DyList.c"
	"Log.c"
	"Lib.c"
	"Server.c"
	"Vote.c"
	"Maps.c"
	"Packet.c"
	"Player.c"
	"Palette.c"
	"Zone.c"
	"Relay.c"
	"Sha256.c"
	"RelayCrypto.c"
	"RelayDns.c"
	"WsProto.c"
	"WsTrust.c"
	"WebGate.c"
	"WebRtc.c"

	"Lobby.c"
	"Mapvote.c"
	"CharSelect.c"
	"Game.c"
	"Results.c"
	"io/Time.c"

	"entities/Ring.c"
	"entities/CreamRing.c"
	"entities/TailsProjectile.c"
	"entities/EggmanTracker.c"
	"entities/BlackRing.c"
	"entities/ExellerClone.c"
	"entities/RMZSlug.c"
	"entities/RMZShard.c"
	"entities/Act9Wall.c"
	"entities/NotPerfect.c"
	"entities/PFLift.c"
	"entities/HDDoor.c"
	"entities/SpkieController.c"
	"entities/LCEye.c"
	"entities/LCChain.c"
	"entities/NAPSnowball.c"
	"entities/NAPIce.c"
	"entities/HillThunder.c"
	"entities/VVLava.c"
	"entities/YouCantRun.c"
	"entities/TCAcid.c"
	"entities/KAFSpeedBox.c"
	"entities/TailsDoll.c"
	"entities/DTBall.c"
	"entities/DTStalactits.c"
	"entities/Dummy.c"
	"entities/WDLatern.c"
	"entities/MJLava.c"
	"entities/MJAss.c"
	"entities/MJJudger.c"

	"maps/RavineMist.c"
	"maps/Act9.c"
	"maps/NotPerfect.c"
	"maps/PricelessFreedom.c"
	"maps/HauntingDream.c"
    "maps/DotDotDot.c"
	"maps/LimpCity.c"
	"maps/NastyParadise.c"
	"maps/Hill.c"
	"maps/VolcanoValley.c"
	"maps/YouCantRun.c"
	"maps/TortureCave.c"
	"maps/KindAndFair.c"
	"maps/DarkTower.c"
	"maps/FartZone.c"
	"maps/MajinForest.c"
	"maps/HideAndSeek.c"
	"maps/WoodDream.c"
	"maps/Marijuna.c"
)

# Lua 5.4 for mods (Mod.c): the core and the safe libraries (no io / os / package / debug)
file(GLOB LUA_SOURCES "${CMAKE_SOURCE_DIR}/lua/*.c")
list(APPEND SOURCES ${LUA_SOURCES})

if(ANDROID)
	add_library(DisasterServer SHARED "Android.c" ${SOURCES})

	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads REQUIRED)
	target_link_libraries(DisasterServer PRIVATE log enet m Threads::Threads)
else()
	if(DYLIB)
		add_library(DisasterServer SHARED ${SOURCES})

		if (WIN32)
			target_link_libraries(DisasterServer PRIVATE enet ws2_32 winmm dbghelp)
		else()
			set(THREADS_PREFER_PTHREAD_FLAG ON)
			find_package(Threads REQUIRED)
			target_link_libraries(DisasterServer PRIVATE enet m Threads::Threads)
		endif()
	else()
		if(BUILD_UI)
			set(UI_SOURCES
				"ui/Resources.c"
				"ui/Main.c"
				"ui/Image.c"
				"ui/Label.c"
				"ui/Button.c"
				"ui/TButton.c"
				"ui/MapList.c"
			  	"ui/Presets.c"
				"ui/MapListPreset.c"
				"ui/PingPreset.c"
				"ui/PlayerList.c"
			)

			if (WIN32)
				find_package(SDL2 CONFIG REQUIRED)
				find_package(SDL2_image CONFIG REQUIRED)
				include_directories(${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS})

				add_executable(DisasterServer
					WIN32
					"icon.rc"
					${SOURCES}
					${UI_SOURCES}
				)

target_link_options(DisasterServer PRIVATE -static -static-libgcc -static-libstdc++)
				target_link_libraries(DisasterServer PRIVATE ws2_32 enet winmm dbghelp $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
					$<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
					$<IF:$<TARGET_EXISTS:SDL2_image::SDL2_image>,SDL2_image::SDL2_image,SDL2_image::SDL2_image-static>)

			else()
				include(FindPkgConfig)
				PKG_SEARCH_MODULE(SDL2 REQUIRED sdl2)
				PKG_SEARCH_MODULE(SDL2IMAGE REQUIRED SDL2_image>=2.0.0)
				include_directories(${SDL2_INCLUDE_DIRS} ${SDL2IMAGE_INCLUDE_DIRS})

				add_executable(DisasterServer
					${SOURCES}
					${UI_SOURCES}
				)

				set(THREADS_PREFER_PTHREAD_FLAG ON)
				find_package(Threads REQUIRED)
				target_link_libraries(DisasterServer PRIVATE enet m Threads::Threads ${SDL2_LIBRARIES} ${SDL2IMAGE_LIBRARIES})
			endif()

		else()
			add_executable(DisasterServer
				${SOURCES}
				Main.c
				AdminCLI.c
			)

			if (WIN32)
target_link_options(DisasterServer PRIVATE -static -static-libgcc -static-libstdc++)
				target_link_libraries(DisasterServer PRIVATE enet ws2_32 winmm dbghelp)
			else()
				set(THREADS_PREFER_PTHREAD_FLAG ON)
				find_package(Threads REQUIRED)
				target_link_libraries(DisasterServer PRIVATE enet m Threads::Threads)
			endif()

		endif()
	endif()
endif()


# Enable link-time optimization for Release builds when supported.
# This keeps the portable build while letting the compiler optimize across C files.
include(CheckIPOSupported)
check_ipo_supported(RESULT DISASTER_IPO_SUPPORTED OUTPUT DISASTER_IPO_ERROR LANGUAGES C)
if(DISASTER_IPO_SUPPORTED)
	set_property(TARGET DisasterServer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if(MSVC)
	add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

unset(DYLIB CACHE)
unset(BUILD_UI CACHE)

# WebRTC data channels for web players (WebRtc.c): libdatachannel + mbedTLS built by
# docker/build_deps.sh into WEBRTC_DEPS. Without it WebGate keeps to WebSockets.
set(WEBRTC_DEPS "" CACHE PATH "libdatachannel + mbedTLS (docker/build_deps.sh); empty: no WebRTC")
if(WEBRTC_DEPS AND TARGET DisasterServer)
	enable_language(CXX)
	target_compile_definitions(DisasterServer PRIVATE HAVE_WEBRTC=1 RTC_STATIC=1)
	target_include_directories(DisasterServer PRIVATE "${WEBRTC_DEPS}/include")
	target_link_libraries(DisasterServer PRIVATE
		"${WEBRTC_DEPS}/lib/libdatachannel.a" "${WEBRTC_DEPS}/lib/libjuice.a" "${WEBRTC_DEPS}/lib/libusrsctp.a"
		"${WEBRTC_DEPS}/lib/libmbedtls.a" "${WEBRTC_DEPS}/lib/libmbedx509.a" "${WEBRTC_DEPS}/lib/libmbedcrypto.a")
	if(WIN32)
		target_link_libraries(DisasterServer PRIVATE iphlpapi bcrypt ws2_32)
	endif()
	set_target_properties(DisasterServer PROPERTIES LINKER_LANGUAGE CXX)
	target_link_options(DisasterServer PRIVATE -static-libstdc++ -static-libgcc)
endif()
