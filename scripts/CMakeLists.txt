find_program(PYTHON python3 REQUIRED)

if (PYTHON)
  # Install main Clam Python scripts
  install(PROGRAMS clam.py DESTINATION bin)
  install(PROGRAMS clam-yaml.py DESTINATION bin)
  install(FILES stats.py DESTINATION bin)
  
  # Install Clam utility scripts
  install(PROGRAMS clam-read-results.py DESTINATION bin)
  install(PROGRAMS clam-debug-assertion.py DESTINATION bin)
  install(PROGRAMS clam-output-check.py DESTINATION bin)
  
  # Install validation scripts
  install(PROGRAMS validate_npd.py DESTINATION bin)
endif()
