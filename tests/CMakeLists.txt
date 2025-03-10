set(PYTHON ${Python3_EXECUTABLE})
set(pyexecdir ${Python3_SITEARCH})
set(usrbindir ${CMAKE_INSTALL_FULL_BINDIR})
set(usrlibdir ${CMAKE_INSTALL_FULL_LIBDIR})
if (${WITH_INTERNAL_OPENPGP})
	if (${WITH_OPENSSL})
		set(CRYPTO openssl)
	else()
		set(CRYPTO libgcrypt)
	endif()
else()
	set(CRYPTO sequoia)
endif()

configure_file(atlocal.in atlocal @ONLY)
configure_file(atconfig.in atconfig @ONLY)
configure_file(package.m4.in package.m4 @ONLY)

set(TESTSUITE_AT
	rpmtests.at
	rpmgeneral.at
	rpmquery.at
	rpmverify.at
	rpmdb.at
	rpmbuild.at
	rpmbuildid.at
	rpmi.at
	rpme.at
	rpmvercmp.at
	rpmdeps.at
	rpmconflict.at
	rpmconfig.at
	rpmconfig2.at
	rpmconfig3.at
	rpmreplace.at
	rpmmacro.at
	rpmpython.at
	rpmdepmatch.at
	rpmscript.at
	rpmsigdig.at
	rpmspec.at
	rpmio.at
	rpmorder.at
	rpmvfylevel.at
	rpmpgp.at
)

set(AUTOTEST ${AUTOM4TE} --language=autotest)
add_custom_command(OUTPUT rpmtests COMMAND
	${AUTOTEST} -I ${CMAKE_CURRENT_SOURCE_DIR} -o rpmtests rpmtests.at
	DEPENDS ${TESTSUITE_AT}
)

# Use our top-level targets as an ordering clue to cmake: the project
# needs to be built before we can populate anything...
get_property(top_targets DIRECTORY .. PROPERTY BUILDSYSTEM_TARGETS)

add_custom_target(populate_testing COMMAND
	${CMAKE_CURRENT_SOURCE_DIR}/populate
		${CMAKE_BINARY_DIR}
		${CMAKE_CURRENT_BINARY_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_INSTALL_FULL_BINDIR}
		${CMAKE_MAKE_PROGRAM}
	SOURCES populate
	DEPENDS ${top_targets}
)

set (testprogs rpmpgpcheck rpmpgppubkeyfingerprint)
foreach(prg ${testprogs})
	add_executable(${prg} EXCLUDE_FROM_ALL ${prg}.c)
	target_link_libraries(${prg} PRIVATE librpmio)
endforeach()

include(ProcessorCount)
ProcessorCount(nproc)
if (nproc GREATER 1)
	set(jobs -j${nproc})
endif()

add_custom_target(check COMMAND
	./rpmtests ${jobs} $(TESTOPTS)
	DEPENDS populate_testing
	DEPENDS rpmtests
	DEPENDS ${testprogs}
)

add_custom_command(TARGET populate_testing POST_BUILD
	COMMAND chmod -R u-w testing
)
add_custom_command(TARGET check POST_BUILD
	COMMAND chmod -R u+w testing
)
