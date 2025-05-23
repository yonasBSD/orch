#
# Copyright (c) 2024 Kyle Evans <kevans@FreeBSD.org>
#
# SPDX-License-Identifier: BSD-2-Clause
#

add_subdirectory(porch)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/porch.lua"
	DESTINATION "${LUA_MODSHAREDIR}")
file(GLOB lua_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/*.lua"
	"${CMAKE_CURRENT_SOURCE_DIR}/porch/*.lua")

add_custom_target(lint-share
	COMMAND echo LINTING FOR LUA 5.3
	COMMAND luacheck --std=lua53 ${lua_SOURCES}
	COMMAND echo LINTING FOR LUA 5.4
	COMMAND luacheck --std=lua54 ${lua_SOURCES}
)
