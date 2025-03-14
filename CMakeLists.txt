cmake_minimum_required(VERSION 3.18)

project(rpm
	VERSION 4.18.90
	DESCRIPTION "The RPM Package Manager"
	HOMEPAGE_URL "http://rpm.org"
	LANGUAGES C
)

# user configurable stuff
option(ENABLE_NLS "Enable native language support" ON)
option(ENABLE_OPENMP "Enable OpenMP threading support" ON)
option(ENABLE_PYTHON "Enable Python bindings" ON)
option(ENABLE_PLUGINS "Enable plugin support" ON)
option(ENABLE_WERROR "Stop build on warnings" OFF)
option(ENABLE_SQLITE "Enable sqlite rpmdb support" ON)
option(ENABLE_NDB "Enable ndb rpmdb support" ON)
option(ENABLE_BDB_RO "Enable read-only Berkeley DB rpmdb support (EXPERIMENTAL)" OFF)
option(ENABLE_TESTSUITE "Enable test-suite" ON)
option(ENABLE_CI "Enable local CI (containerized test-suite)" OFF)

option(WITH_INTERNAL_OPENPGP "Use internal OpenPGP parse (DEPRECATED)" OFF)
option(WITH_OPENSSL "Use openssl (instead of libgcrypt) for internal crypto" OFF)

option(WITH_CAP "Build with capability support" ON)
option(WITH_ACL "Build with ACL support" ON)
option(WITH_ARCHIVE "Build with libarchive support" ON)
option(WITH_SELINUX "Build with SELinux support" ON)
option(WITH_DBUS "Build with DBUS support" ON)
option(WITH_AUDIT "Build with audit support" ON)
option(WITH_FSVERITY "Build with fsverity support" OFF)
option(WITH_IMAEVM "Build with IMA support" OFF)
option(WITH_FAPOLICYD "Build with fapolicyd support" ON)

set(RPM_CONFIGDIR "${CMAKE_INSTALL_PREFIX}/lib/rpm" CACHE PATH "rpm home")
set(RPM_VENDOR "vendor" CACHE STRING "rpm vendor string")

# Emulate libtool versioning. Before a public release:
# - increment micro version whenever there are code changes
# - increment minor version whenever there are added interfaces
# - increment major version whenever there are removed interfaces
# - incrementing a more significant version segment resets changes to
#   any less significant segments
set(RPM_SOVERSION 10)
set(RPM_LIBVERSION ${RPM_SOVERSION}.0.0)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_SHARED_MODULE_PREFIX "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(GNUInstallDirs)
add_compile_definitions(_GNU_SOURCE)

function(makemacros)
	set(prefix ${CMAKE_INSTALL_PREFIX})
	set(exec_prefix "\${prefix}")
	set(bindir "\${exec_prefix}/${CMAKE_INSTALL_BINDIR}")
	set(sbindir "\${exec_prefix}/${CMAKE_INSTALL_SBINDIR}")
	set(libexecdir "\${exec_prefix}/${CMAKE_INSTALL_LIBEXECDIR}")
	set(datarootdir "\${prefix}/${CMAKE_INSTALL_DATAROOTDIR}")
	set(datadir "\${datarootdir}")
	set(sysconfdir "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
	set(sharedstatedir "${CMAKE_INSTALL_FULL_SHAREDSTATEDIR}")
	set(localstatedir "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}")
	set(libdir "\${prefix}/=LIB=")
	set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
	set(oldincludedir "${CMAKE_INSTALL_FULL_OLDINCLUDEDIR}")
	set(infodir "\${datarootdir}/${CMAKE_INSTALL_INFODIR}")
	set(mandir "\${datarootdir}/${CMAKE_INSTALL_MANDIR}")
	set(RUNDIR /run)

	set(acutils
		awk ar as cpp cxx
	)
	foreach (V ${acutils})
		string(TOUPPER ${V} n)
		set(${n} /usr/bin/${V})
	endforeach()
	set(extutils
		7zip bzip2 cat chmod chown cp curl file gpg grep gzip id cc ln
		install lrzip lzip xz make mkdir mv patch rm sed tar unzip
		zstd gem git hg bzr quilt ld objdump strip
	)
	foreach (util ${extutils})
		string(TOUPPER ${util} UTIL)
		find_program(__${UTIL} ${util})
		if (NOT EXISTS ${__${UTIL}})
			message(DEBUG "${util} not found, assuming /usr/bin")
			set(__${UTIL} /usr/bin/${util})
		endif()
	endforeach()

	list(GET db_backends 0 DB_BACKEND)

	set(host_cpu ${CMAKE_HOST_SYSTEM_PROCESSOR})
	string(TOLOWER ${CMAKE_HOST_SYSTEM_NAME} host_os)
	set(host_vendor ${RPM_VENDOR})
	set(host ${host_cpu}-${host_vendor}-${host_os})

	set(RPMCANONVENDOR ${host_vendor})
	set(RPMCANONOS ${host_os})
	set(RPMCANONGNU -gnu)

	configure_file(platform.in platform @ONLY)
	configure_file(rpmrc.in rpmrc @ONLY)
	configure_file(macros.in macros @ONLY)
	configure_file(rpmpopt.in rpmpopt-${PROJECT_VERSION} @ONLY)
	configure_file(rpm.pc.in rpm.pc @ONLY)

	install(CODE "execute_process(COMMAND
		${CMAKE_COMMAND} -E env pkglibdir=${RPM_CONFIGDIR}
			${CMAKE_SOURCE_DIR}/installplatform
			rpmrc platform macros
			${RPMCANONVENDOR} ${RPMCANONOS} ${RPMCANONGNU})"
	)
endfunction()

include(CheckLibraryExists)
include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckCCompilerFlag)

set(OPTFUNCS
	stpcpy stpncpy putenv mempcpy fdatasync lutimes mergesort
	getauxval setprogname __progname syncfs sched_getaffinity unshare
	secure_getenv __secure_getenv mremap
)
set(REQFUNCS
	mkstemp getcwd basename dirname realpath setenv unsetenv regcomp
	utimes getline localtime_r statvfs getaddrinfo
	openat mkdirat fstatat linkat symlinkat mkfifoat mknodat unlinkat
	renameat utimensat fchmodat fchownat stpcpy stpncpy
)

find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)
find_package(BZip2)
find_package(Iconv)

pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua>=5.2)
pkg_check_modules(POPT REQUIRED IMPORTED_TARGET popt)
pkg_check_modules(READLINE IMPORTED_TARGET readline)
pkg_check_modules(ZSTD IMPORTED_TARGET libzstd>=1.3.8)
pkg_check_modules(LIBELF IMPORTED_TARGET libelf)
pkg_check_modules(LIBDW IMPORTED_TARGET libdw)
pkg_check_modules(LIBLZMA IMPORTED_TARGET liblzma>=5.2.0)

# file >= 5.39 ships a pkg-config, may move to that later
add_library(MAGIC::MAGIC UNKNOWN IMPORTED)
find_library(MAGIC_LIBRARY NAMES magic REQUIRED)
find_path(MAGIC_INCLUDE_DIR NAMES magic.h REQUIRED)
set_target_properties(MAGIC::MAGIC PROPERTIES
		      IMPORTED_LOCATION "${MAGIC_LIBRARY}")
target_include_directories(MAGIC::MAGIC INTERFACE "${MAGIC_INCLUDE_DIR}")

if (ENABLE_OPENMP)
	find_package(OpenMP 4.5 REQUIRED)
endif()

if (ENABLE_NLS)
	find_package(Intl REQUIRED)
endif()

if (ENABLE_SQLITE)
	pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3>=3.22)
	list(APPEND db_backends sqlite)
endif()

if (ENABLE_NDB)
	list(APPEND db_backends ndb)
endif()

if (ENABLE_BDB_RO)
	list(APPEND db_backends bdb_ro)
endif()
list(APPEND db_backends dummy)

if (ENABLE_PYTHON)
	find_package(Python3 3.2 COMPONENTS Interpreter Development REQUIRED)
endif()

if (WITH_CAP)
	pkg_check_modules(LIBCAP REQUIRED IMPORTED_TARGET libcap)
endif()

if (WITH_ACL)
	pkg_check_modules(LIBACL REQUIRED IMPORTED_TARGET libacl)
endif()

if (WITH_AUDIT)
	pkg_check_modules(AUDIT REQUIRED IMPORTED_TARGET audit)
endif()

if (WITH_SELINUX)
	pkg_check_modules(SELINUX REQUIRED IMPORTED_TARGET libselinux)
endif()

if (WITH_ARCHIVE)
	pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
endif()

if (WITH_FSVERITY)
	pkg_check_modules(FSVERITY REQUIRED IMPORTED_TARGET libfsverity)
endif()

if (WITH_IMAEVM)
	list(APPEND REQFUNCS lsetxattr)
	add_library(IMA::IMA UNKNOWN IMPORTED)
	find_path(IMA_INCLUDE_DIR NAMES imaevm.h REQUIRED)
	find_library(IMA_LIBRARY NAMES imaevm REQUIRED)
	set_target_properties(IMA::IMA PROPERTIES
			      IMPORTED_LOCATION "${IMA_LIBRARY}")
	target_include_directories(IMA::IMA INTERFACE "${IMA_INCLUDE_DIR}")
endif()

find_program(__FIND_DEBUGINFO find-debuginfo)

function(chkdef func req)
	string(TOUPPER ${func} FUNC)
	set(HAVENAME HAVE_${FUNC})
	check_function_exists(${func} ${HAVENAME})
	if (${req} AND NOT ${HAVENAME})
		message(FATAL_ERROR "required function ${func} not found")
	endif()
endfunction()

foreach(f ${OPTFUNCS})
    chkdef(${f} FALSE)
endforeach()

foreach(f ${REQFUNCS})
    chkdef(${f} TRUE)
endforeach()

function(chkhdr inc req)
	string(MAKE_C_IDENTIFIER ${inc} ID)
	string(TOUPPER ${ID} INC)
	set(HAVENAME HAVE_${INC})
	check_include_file(${inc} ${HAVENAME})
	if (${req} AND NOT ${HAVENAME})
		message(FATAL_ERROR "required include ${inc} not found")
	endif()
endfunction()

set(OPTINCS
	unistd.h limits.h getopt.h
	sys/utsname.h sys/systemcfg.h sys/param.h sys/auxv.h
)
foreach(f ${OPTINCS})
    chkhdr(${f} FALSE)
endforeach()

function(id0name var file)
	execute_process(COMMAND awk -F: "$3==0 {print $1;exit}" ${file}
			OUTPUT_STRIP_TRAILING_WHITESPACE
			OUTPUT_VARIABLE name)
	if ("${name}" STREQUAL "")
		set(name root)
	endif()
	set(${var} ${name} PARENT_SCOPE)
endfunction()

id0name(UID_0_USER /etc/passwd)
id0name(GID_0_GROUP /etc/group)

# map module/package findings to config.h
if (${Bzip2_FOUND})
	set(HAVE_BZLIB_H 1)
endif()
if (${LIBLZMA_FOUND})
	set(HAVE_LZMA_H 1)
endif()
if (${Iconv_FOUND})
	set(HAVE_ICONV 1)
endif()
foreach(found ZSTD READLINE LIBELF LIBDW)
	if (${${found}_FOUND})
		set(HAVE_${found} 1)
	endif()
endforeach()

check_symbol_exists(major "sys/sysmacros.h" MAJOR_IN_SYSMACROS)
if (NOT MAJOR_IN_SYSMACROS)
	check_symbol_exists(major "sys/mkdev.h" MAJOR_IN_MKDEV)
endif()

configure_file(config.h.in config.h)

add_compile_definitions(HAVE_CONFIG_H)

add_compile_options(-Wall -Wpointer-arith -Wmissing-prototypes -Wstrict-prototypes -Wempty-body)
if (ENABLE_WERROR)
	add_compile_options(-Werror)
endif()

# try to ensure some compiler sanity
foreach (flag -fno-strict-overflow -fno-delete-null-pointer-checks)
	check_c_compiler_flag(${flag} found)
	if (found)
		add_compile_options(${flag})
	endif()
	unset(found)
endforeach()

include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)

add_subdirectory(docs)
add_subdirectory(include/rpm)
add_subdirectory(misc)
add_subdirectory(rpmio)
add_subdirectory(lib)
add_subdirectory(build)
add_subdirectory(sign)

if (EXISTS ${CMAKE_SOURCE_DIR}/po/rpm.pot)
	add_subdirectory(po)
endif()

if (ENABLE_PYTHON)
	add_subdirectory(python)
endif()

if (ENABLE_PLUGINS)
	add_subdirectory(plugins)
endif()


add_subdirectory(fileattrs)
add_subdirectory(scripts)

add_library(cliutils OBJECT cliutils.c cliutils.h)

add_executable(rpm rpm.c cliutils)
add_executable(rpmdb rpmdb.c cliutils)
add_executable(rpmkeys rpmkeys.c cliutils)
add_executable(rpm2cpio rpm2cpio.c cliutils)
add_executable(rpmsign rpmsign.c cliutils)
add_executable(rpmbuild rpmbuild.c cliutils)
add_executable(rpmspec rpmspec.c cliutils)

add_executable(rpmdeps tools/rpmdeps.c)
add_executable(rpmgraph tools/rpmgraph.c)
add_executable(rpmlua tools/rpmlua.c)
add_executable(rpmsort tools/rpmsort.c)
add_executable(rpmuncompress tools/rpmuncompress.c)
add_executable(elfdeps tools/elfdeps.c)

target_link_libraries(rpmsign PRIVATE librpmsign)
target_link_libraries(rpmlua PRIVATE PkgConfig::LUA)
target_link_libraries(elfdeps PRIVATE PkgConfig::LIBELF)
target_link_libraries(rpmbuild PRIVATE librpmbuild)
target_link_libraries(rpmspec PRIVATE librpmbuild)
target_link_libraries(rpmdeps PRIVATE librpmbuild)

if (READLINE_FOUND)
	target_link_libraries(rpmspec PRIVATE PkgConfig::READLINE)
	target_link_libraries(rpmlua PRIVATE PkgConfig::READLINE)
endif()

if (WITH_ARCHIVE)
	add_executable(rpm2archive rpm2archive.c)
	target_link_libraries(rpm2archive PRIVATE PkgConfig::LIBARCHIVE)
	install(TARGETS rpm2archive)
endif()

# Everything links to these
get_property(executables DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(exe ${executables})
	target_link_libraries(${exe} PRIVATE librpmio librpm PkgConfig::POPT)
endforeach()

foreach(cmd rpmverify rpmquery)
	add_custom_target(${cmd} ALL COMMAND
			${CMAKE_COMMAND} -E create_symlink rpm ${cmd}
			BYPRODUCTS ${cmd})
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${cmd} TYPE BIN)
endforeach()
install(TARGETS
	rpm rpmdb rpmkeys rpm2cpio rpmsign rpmbuild rpmspec
	rpmlua rpmgraph rpmsort
)
install(TARGETS elfdeps rpmdeps rpmuncompress DESTINATION ${RPM_CONFIGDIR})

makemacros()
foreach(f macros rpmrc rpmpopt-${PROJECT_VERSION})
	install(FILES ${CMAKE_BINARY_DIR}/${f} DESTINATION ${RPM_CONFIGDIR})
endforeach()

if (ENABLE_TESTSUITE)
	find_program(AUTOM4TE autom4te REQUIRED)
	find_program(FAKECHROOT fakechroot REQUIRED)
	add_subdirectory(tests)
	if (ENABLE_CI)
		find_program(PODMAN podman docker REQUIRED)
		add_subdirectory(ci)
	endif()
endif()

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rpm.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(DIRECTORY DESTINATION ${RPM_CONFIGDIR}/lua)
install(DIRECTORY DESTINATION ${RPM_CONFIGDIR}/macros.d)
install(FILES CONTRIBUTING.md COPYING CREDITS INSTALL README TYPE DOC)

add_custom_target(ChangeLog
		   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		   COMMAND git log --no-merges
				--output=${CMAKE_BINARY_DIR}/ChangeLog
)

function(add_tarball targetname namever treeish)
	set(distfmt tar)
	set(tarname ${namever}.${distfmt})
	set(distname ${tarname}.bz2)
	set(docname ${namever}-doc.${distfmt})

	add_custom_target(${docname}
		DEPENDS man apidoc
		COMMAND tar
			-C ${CMAKE_BINARY_DIR}
			--transform 's:^:${namever}/:'
			-cf ${docname} docs/man/*.[1-8] docs/html/
	)

	add_custom_target(${distname}
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		BYPRODUCTS ${distname} ${docname}
		VERBATIM
		DEPENDS ChangeLog ${docname}
		COMMAND git archive
			--format=${distfmt}
			--output=${CMAKE_BINARY_DIR}/${tarname}
			--prefix=${namever}/
			--add-file=${CMAKE_BINARY_DIR}/ChangeLog
			${treeish}
		COMMAND git submodule foreach --quiet
			"git archive --prefix=${namever}/$sm_path/ \
				--output=${CMAKE_BINARY_DIR}/$sha1.tar HEAD \
			 && tar --concatenate \
				--file=${CMAKE_BINARY_DIR}/${tarname} \
				${CMAKE_BINARY_DIR}/$sha1.tar \
			 && rm -f ${CMAKE_BINARY_DIR}/$sha1.tar"
		COMMAND tar --concatenate
				--file=${CMAKE_BINARY_DIR}/${tarname}
				${CMAKE_BINARY_DIR}/${docname}
		COMMAND bzip2 -f ${CMAKE_BINARY_DIR}/${tarname}
	)
	add_custom_target(${targetname} DEPENDS ${distname})
endfunction()

add_tarball(dist ${PROJECT_NAME}-${PROJECT_VERSION} HEAD)

if (EXISTS ${CMAKE_SOURCE_DIR}/.git)
	execute_process(COMMAND git rev-list --count HEAD
			WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
			OUTPUT_VARIABLE gitcount
			OUTPUT_STRIP_TRAILING_WHITESPACE)
	add_tarball(snapshot
		    ${PROJECT_NAME}-${PROJECT_VERSION}-git${gitcount} HEAD)
endif()
