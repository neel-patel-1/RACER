#!/bin/bash

#!/bin/bash

PLACEMENTS=( 0 1 2 3 4 5 6 )
PLACEMENT_NAMES=( L2D L2C LLC DDR CXL CXL_DDR DDR_CXL )
PAYLOAD_SIZES=( 256 1024 4096 16384 65536 262144 1048576 $(( 2 * 1024 * 1024 )) $(( 4 * 1024 * 1024 ))  $(( 8 * 1024 * 1024 )) $(( 16 * 1024 * 1024 )) $(( 32 * 1024 * 1024 )) $(( 64 * 1024 * 1024 )) $(( 128 * 1024 * 1024 )) $(( 256 * 1024 * 1024 )) $(( 512 * 1024 * 1024 )) $(( 1024 * 1024 * 1024 )) $(( 2 * 1024 * 1024 * 1024 )) )

# print header with sizes in KB (human-friendly)
printf "PE\tPlacement"
for s in "${PAYLOAD_SIZES[@]}"; do
  # divide by 1024 and print without unnecessary zeros
  printf "\t%s" "$(awk -v v="$s" 'BEGIN{ f=v/1024; if (f==int(f)) printf("%d",f); else printf("%g",f)}')"
done
printf "\n"

printf "DSA_Memmove"
for p_idx in ${!PLACEMENTS[@]}; do
  placement=${PLACEMENTS[$p_idx]}
  placement_name=${PLACEMENT_NAMES[$p_idx]}
  printf "\t${placement_name}"
  for payload_size in "${PAYLOAD_SIZES[@]}"; do
    log="logs/placement_dsa_3_${payload_size}_cstate_${placement}.log"
    if [[ ! -f "$log" ]]; then
      val="-"
    else
      val=$(grep "Block" "$log" | awk '{print $5}')
    fi
    printf "\t%s" "$val"
  done
  echo
done
