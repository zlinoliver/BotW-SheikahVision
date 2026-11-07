# configure_file updates the stamp only when manifest contents change, regardless of archive mtimes.
function(botw_runtime_stamp target kit)
  configure_file("${kit}/runtime.json" "${CMAKE_BINARY_DIR}/runtime-kit.stamp" COPYONLY)
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_BINARY_DIR}/runtime-kit.stamp" "${kit}/libsheikah_runtime.a" "${kit}/libbotw_immersive.a")
endfunction()
