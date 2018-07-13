# NOTE: Assertions have been autogenerated by utils/update_mca_test_checks.py
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2 -all-stats       < %s | FileCheck %s -check-prefix=ALL -check-prefix=FULLREPORT
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2 -all-stats=true  < %s | FileCheck %s -check-prefix=ALL -check-prefix=FULLREPORT
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2 -all-stats=false < %s | FileCheck %s -check-prefix=ALL
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2                  < %s | FileCheck %s -check-prefix=ALL

add %eax, %eax

# FULLREPORT:      Iterations:        100
# FULLREPORT-NEXT: Instructions:      100
# FULLREPORT-NEXT: Total Cycles:      103
# FULLREPORT-NEXT: Dispatch Width:    2
# FULLREPORT-NEXT: IPC:               0.97
# FULLREPORT-NEXT: Block RThroughput: 0.5

# ALL:             Instruction Info:
# ALL-NEXT:        [1]: #uOps
# ALL-NEXT:        [2]: Latency
# ALL-NEXT:        [3]: RThroughput
# ALL-NEXT:        [4]: MayLoad
# ALL-NEXT:        [5]: MayStore
# ALL-NEXT:        [6]: HasSideEffects

# ALL:             [1]    [2]    [3]    [4]    [5]    [6]    Instructions:
# ALL-NEXT:         1      1     0.50                        addl	%eax, %eax

# FULLREPORT:      Dynamic Dispatch Stall Cycles:
# FULLREPORT-NEXT: RAT     - Register unavailable:                      0
# FULLREPORT-NEXT: RCU     - Retire tokens unavailable:                 0
# FULLREPORT-NEXT: SCHEDQ  - Scheduler full:                            61
# FULLREPORT-NEXT: LQ      - Load queue full:                           0
# FULLREPORT-NEXT: SQ      - Store queue full:                          0
# FULLREPORT-NEXT: GROUP   - Static restrictions on the dispatch group: 0

# FULLREPORT:      Dispatch Logic - number of cycles where we saw N instructions dispatched:
# FULLREPORT-NEXT: [# dispatched], [# cycles]
# FULLREPORT-NEXT:  0,              22  (21.4%)
# FULLREPORT-NEXT:  1,              62  (60.2%)
# FULLREPORT-NEXT:  2,              19  (18.4%)

# FULLREPORT:      Schedulers - number of cycles where we saw N instructions issued:
# FULLREPORT-NEXT: [# issued], [# cycles]
# FULLREPORT-NEXT:  0,          3  (2.9%)
# FULLREPORT-NEXT:  1,          100  (97.1%)

# FULLREPORT:      Scheduler's queue usage:
# FULLREPORT-NEXT: JALU01,  20/20
# FULLREPORT-NEXT: JFPU01,  0/18
# FULLREPORT-NEXT: JLSAGU,  0/12

# FULLREPORT:      Retire Control Unit - number of cycles where we saw N instructions retired:
# FULLREPORT-NEXT: [# retired], [# cycles]
# FULLREPORT-NEXT:  0,           3  (2.9%)
# FULLREPORT-NEXT:  1,           100  (97.1%)

# FULLREPORT:      Register File statistics:
# FULLREPORT-NEXT: Total number of mappings created:    200
# FULLREPORT-NEXT: Max number of mappings used:         44

# FULLREPORT:      *  Register File #1 -- JFpuPRF:
# FULLREPORT-NEXT:    Number of physical registers:     72
# FULLREPORT-NEXT:    Total number of mappings created: 0
# FULLREPORT-NEXT:    Max number of mappings used:      0

# FULLREPORT:      *  Register File #2 -- JIntegerPRF:
# FULLREPORT-NEXT:    Number of physical registers:     64
# FULLREPORT-NEXT:    Total number of mappings created: 200
# FULLREPORT-NEXT:    Max number of mappings used:      44

# FULLREPORT:      Resources:
# FULLREPORT-NEXT: [0]   - JALU0
# FULLREPORT-NEXT: [1]   - JALU1
# FULLREPORT-NEXT: [2]   - JDiv
# FULLREPORT-NEXT: [3]   - JFPA
# FULLREPORT-NEXT: [4]   - JFPM
# FULLREPORT-NEXT: [5]   - JFPU0
# FULLREPORT-NEXT: [6]   - JFPU1
# FULLREPORT-NEXT: [7]   - JLAGU
# FULLREPORT-NEXT: [8]   - JMul
# FULLREPORT-NEXT: [9]   - JSAGU
# FULLREPORT-NEXT: [10]  - JSTC
# FULLREPORT-NEXT: [11]  - JVALU0
# FULLREPORT-NEXT: [12]  - JVALU1
# FULLREPORT-NEXT: [13]  - JVIMUL

# FULLREPORT:      Resource pressure per iteration:
# FULLREPORT-NEXT: [0]    [1]    [2]    [3]    [4]    [5]    [6]    [7]    [8]    [9]    [10]   [11]   [12]   [13]
# FULLREPORT-NEXT: 0.50   0.50    -      -      -      -      -      -      -      -      -      -      -      -

# FULLREPORT:      Resource pressure by instruction:
# FULLREPORT-NEXT: [0]    [1]    [2]    [3]    [4]    [5]    [6]    [7]    [8]    [9]    [10]   [11]   [12]   [13]   Instructions:
# FULLREPORT-NEXT: 0.50   0.50    -      -      -      -      -      -      -      -      -      -      -      -     addl	%eax, %eax