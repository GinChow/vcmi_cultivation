
#message(${CMAKE_ARGV0}) # cmake.exe
#message(${CMAKE_ARGV1}) # -P
#message(${CMAKE_ARGV2}) # thisfilename
#message(${CMAKE_ARGV3}) # existing
#message(${CMAKE_ARGV4}) # linkname
if(WIN32)
	file(TO_NATIVE_PATH "${CMAKE_ARGV3}" existing_native)
	file(TO_NATIVE_PATH "${CMAKE_ARGV4}" linkname_native)

	if(EXISTS "${CMAKE_ARGV4}" OR IS_SYMLINK "${CMAKE_ARGV4}")
		execute_process(COMMAND cmd.exe /c RD /Q "${linkname_native}")
	endif()

	execute_process(COMMAND cmd.exe /c mklink /J "${linkname_native}" "${existing_native}")
else()
	# 链接已经指向这里就不动它：create_symlink 是先删再建，正在运行的游戏按需读图时（Mods 里的图是边用边读的），
	# 恰好碰上删掉的那一瞬会读不到文件，引擎不重试，那几帧在这局里一直是空的，动画就一闪一闪（2026-11-20 实机）
	if(IS_SYMLINK "${CMAKE_ARGV4}")
		file(READ_SYMLINK "${CMAKE_ARGV4}" current_target)
		if(current_target STREQUAL "${CMAKE_ARGV3}")
			return()
		endif()
	endif()
	execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_ARGV3}" "${CMAKE_ARGV4}")
endif()