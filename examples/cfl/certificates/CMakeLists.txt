foreach(example IN ITEMS DyckCertificateExample CFGCertificateExample
                         TwoNPDACertificateExample)
  lotus_add_example(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE CanaryCFLCertificates)
endforeach()
