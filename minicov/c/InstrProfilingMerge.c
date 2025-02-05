/*===- InstrProfilingMerge.c - Profile in-process Merging  ---------------===*\
|*
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
|* See https://llvm.org/LICENSE.txt for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*
|* This file defines the API needed for in-process merging of profile data
|* stored in memory buffer.
\*===---------------------------------------------------------------------===*/

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

#define INSTR_PROF_VALUE_PROF_DATA
#include "profile/InstrProfData.inc"

COMPILER_RT_VISIBILITY
void (*VPMergeHook)(ValueProfData *, __llvm_profile_data *);

COMPILER_RT_VISIBILITY
uint64_t lprofGetLoadModuleSignature() {
  /* A very fast way to compute a module signature.  */
  uint64_t Version = __llvm_profile_get_version();
  uint64_t CounterSize = (uint64_t)(__llvm_profile_end_counters() -
                                    __llvm_profile_begin_counters());
  uint64_t DataSize = __llvm_profile_get_data_size(__llvm_profile_begin_data(),
                                                   __llvm_profile_end_data());
  uint64_t NamesSize =
      (uint64_t)(__llvm_profile_end_names() - __llvm_profile_begin_names());
  uint64_t NumVnodes =
      (uint64_t)(__llvm_profile_end_vnodes() - __llvm_profile_begin_vnodes());
  const __llvm_profile_data *FirstD = __llvm_profile_begin_data();

  return (NamesSize << 40) + (CounterSize << 30) + (DataSize << 20) +
         (NumVnodes << 10) + (DataSize > 0 ? FirstD->NameRef : 0) + Version;
}

/* Returns 1 if profile is not structurally compatible.  */
COMPILER_RT_VISIBILITY
int __llvm_profile_check_compatibility(const char *ProfileData,
                                       uint64_t ProfileSize) {
  /* Check profile header only for now  */
  __llvm_profile_header *Header = (__llvm_profile_header *)ProfileData;
  __llvm_profile_data *SrcDataStart, *SrcDataEnd, *SrcData, *DstData;
  SrcDataStart =
      (__llvm_profile_data *)(ProfileData + sizeof(__llvm_profile_header) +
                              Header->BinaryIdsSize);
  SrcDataEnd = SrcDataStart + Header->DataSize;

  if (ProfileSize < sizeof(__llvm_profile_header))
    return 1;

  /* Check the header first.  */
  if (Header->Magic != __llvm_profile_get_magic() ||
      Header->Version != __llvm_profile_get_version() ||
      Header->DataSize !=
          __llvm_profile_get_data_size(__llvm_profile_begin_data(),
                                       __llvm_profile_end_data()) ||
      Header->CountersSize != (uint64_t)(__llvm_profile_end_counters() -
                                         __llvm_profile_begin_counters()) ||
      Header->NamesSize != (uint64_t)(__llvm_profile_end_names() -
                                      __llvm_profile_begin_names()) ||
      Header->ValueKindLast != IPVK_Last)
    return 1;

  if (ProfileSize < sizeof(__llvm_profile_header) + Header->BinaryIdsSize +
                        Header->DataSize * sizeof(__llvm_profile_data) +
                        Header->NamesSize + Header->CountersSize)
    return 1;

  for (SrcData = SrcDataStart,
       DstData = (__llvm_profile_data *)__llvm_profile_begin_data();
       SrcData < SrcDataEnd; ++SrcData, ++DstData) {
    if (SrcData->NameRef != DstData->NameRef ||
        SrcData->FuncHash != DstData->FuncHash ||
        SrcData->NumCounters != DstData->NumCounters)
      return 1;
  }

  /* Matched! */
  return 0;
}

COMPILER_RT_VISIBILITY
int __llvm_profile_merge_from_buffer(const char *ProfileData,
                                     uint64_t ProfileSize) {
  __llvm_profile_data *SrcDataStart, *SrcDataEnd, *SrcData, *DstData;
  __llvm_profile_header *Header = (__llvm_profile_header *)ProfileData;
  uint64_t *SrcCountersStart;
  const char *SrcNameStart;
  const char *SrcValueProfDataStart, *SrcValueProfData;

  SrcDataStart =
      (__llvm_profile_data *)(ProfileData + sizeof(__llvm_profile_header) +
                              Header->BinaryIdsSize);
  SrcDataEnd = SrcDataStart + Header->DataSize;
  SrcCountersStart = (uint64_t *)SrcDataEnd;
  SrcNameStart = (const char *)(SrcCountersStart + Header->CountersSize);
  SrcValueProfDataStart =
      SrcNameStart + Header->NamesSize +
      __llvm_profile_get_num_padding_bytes(Header->NamesSize);
  if (SrcNameStart < (const char *)SrcCountersStart)
    return 1;

  for (SrcData = SrcDataStart,
      DstData = (__llvm_profile_data *)__llvm_profile_begin_data(),
      SrcValueProfData = SrcValueProfDataStart;
       SrcData < SrcDataEnd; ++SrcData, ++DstData) {
    uint64_t *DstCounters = (uint64_t *)DstData->CounterPtr;
    unsigned NVK = 0;

    unsigned NC = SrcData->NumCounters;
    if (NC == 0)
      return 1;
    uint64_t *SrcCounters = SrcCountersStart + ((size_t)SrcData->CounterPtr -
                                                Header->CountersDelta) /
                                                   sizeof(uint64_t);
    if (SrcCounters < SrcCountersStart ||
        (const char *)SrcCounters >= SrcNameStart ||
        (const char *)(SrcCounters + NC) > SrcNameStart)
      return 1;
    for (unsigned I = 0; I < NC; I++)
      DstCounters[I] += SrcCounters[I];

    /* Now merge value profile data. */
    if (!VPMergeHook)
      continue;

    for (unsigned I = 0; I <= IPVK_Last; I++)
      NVK += (SrcData->NumValueSites[I] != 0);

    if (!NVK)
      continue;

    if (SrcValueProfData >= ProfileData + ProfileSize)
      return 1;
    VPMergeHook((ValueProfData *)SrcValueProfData, DstData);
    SrcValueProfData =
        SrcValueProfData + ((ValueProfData *)SrcValueProfData)->TotalSize;
  }

  return 0;
}
