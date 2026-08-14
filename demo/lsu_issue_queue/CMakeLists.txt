project(lsu_issue_queue_demo)

add_executable(lsu_issue_queue_demo
  LsuIssueQueueDemo.cpp
  ${SIM_BASE}/sim/OlympiaSim.cpp
)

target_link_libraries(lsu_issue_queue_demo
  core
  mss
  SPARTA::sparta
  stf
)
