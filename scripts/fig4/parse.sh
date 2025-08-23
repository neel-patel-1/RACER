#!/bin/bash

#!/bin/bash

PAYLOAD_SIZES=( 256 1024 4096 16384 65536 262144 1048576 )

# print header with sizes in KB (human-friendly)
printf "PE\tPlacement\t"
for s in "${PAYLOAD_SIZES[@]}"; do
  # divide by 1024 and print without unnecessary zeros
  printf "\t%s" "$(awk -v v="$s" 'BEGIN{ f=v/1024; if (f==int(f)) printf("%d",f); else printf("%g",f)}')"
done
printf "\n"

# helper: collect values per-cache-state (assumes 4 cache-state files per pattern, grep_word selects Baseline/Block)
# args: pattern_prefix grep_word
collect_rows() {
  local pattern="$1" grep_word="$2"
  # init rows
  local rows0="" rows1="" rows2="" rows3=""
  for j in "${PAYLOAD_SIZES[@]}"; do
    # gather the $5 field from matching files (keeps same order as files -> cache states)
    vals=$(for i in logs/${pattern}_${j}_cstate_*; do
            grep "$grep_word" "$i" 2>/dev/null
          done | awk '{printf("%s ", $5)}')
    # split into array
    read -r -a arr <<< "$vals"
    # ensure 4 entries (fill missing with '-')
    for k in ${!arr[@]}; do :; done
    arr[0]=${arr[0]:--}
    arr[1]=${arr[1]:--}
    arr[2]=${arr[2]:--}
    arr[3]=${arr[3]:--}
    rows0="${rows0}\t${arr[0]}"
    rows1="${rows1}\t${arr[1]}"
    rows2="${rows2}\t${arr[2]}"
    rows3="${rows3}\t${arr[3]}"
  done
  # emit rows (tab-prefixed to align under the left label)
  printf "%s\n" "$rows0"
  printf "%s\n" "$rows1"
  printf "%s\n" "$rows2"
  printf "%s\n" "$rows3"
}

# print DSA block results (Block entries from placement_dsa_3_ -> memcpy)
NAMES=( "dsa" "dsa" "iaa" )
APPS=( 8 8 9 )
OPCODES=( 3 4 66 )
PLACEMENTS=( 0 1 2 3 4 )

for app_idx in ${!APPS[@]}; do
  name=${NAMES[$app_idx]}
  app=${APPS[$app_idx]}
  opcode=${OPCODES[$app_idx]}
  printf "${name}-${opcode}\t"
  read -r row0 row1 row2 row3 < <(collect_rows "placement_${name}_${opcode}" "Block")
  # collect_rows already printed rows; but we need them returned — change to capture:
  # workaround: re-call but capture output properly
  rows=$(collect_rows "placement_${name}_${opcode}" "Block")
  # print with labels
  awk -v r="$rows" 'BEGIN{ split(r,lines,"\n"); printf("\tL2D%s\n\tL2C%s\n\tLLC%s\n\tDRAM%s\n", lines[1], lines[2], lines[3], lines[4]) }'

  # print gpCore (Baseline entries from same logs)
  printf "gpCore\t"
  rows=$(collect_rows "placement_${name}_${opcode}" "Baseline")
  awk -v r="$rows" 'BEGIN{ split(r,lines,"\n"); printf("\tL2D%s\n\tL2C%s\n\tLLC%s\n\tDRAM%s\n", lines[1], lines[2], lines[3], lines[4]) }'
done