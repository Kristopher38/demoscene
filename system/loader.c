#include <debug.h>
#include <custom.h>
#include <string.h>
#include <system/amigahunk.h>
#include <system/autoinit.h>
#include <system/boot.h>
#include <system/cia.h>
#include <system/cpu.h>
#include <system/exception.h>
#include <system/filesys.h>
#include <system/file.h>
#include <system/floppy.h>
#include <system/interrupt.h>
#include <system/memfile.h>
#include <system/memory.h>
#include <system/task.h>

u_char CpuModel = CPU_68000;

extern int main(void);
extern u_char JumpTable[];
extern u_char JumpTableSize[];

void Loader(BootDataT *bd) {
  Log("[Loader] VBR at $%08x\n", (u_int)bd->bd_vbr);
  Log("[Loader] CPU model $%02x\n", bd->bd_cpumodel);
  Log("[Loader] Stack at $%08x (%d bytes)\n",
      (u_int)bd->bd_stkbot, bd->bd_stksz);

#ifndef AMIGAOS
  Log("[Loader] Executable file segments:\n");
  {
    HunkT *hunk = bd->bd_hunk;
    do {
      Log("[Loader] * $%08x - $%08lx\n",
          (u_int)hunk->data, (u_int)hunk->data + hunk->size - sizeof(HunkT));
      hunk = hunk->next;
    } while (hunk);
  }
#endif

  CpuModel = bd->bd_cpumodel;
  ExcVecBase = bd->bd_vbr;

  {
    short i;

    for (i = 0; i < bd->bd_nregions; i++) {
      MemRegionT *mr = &bd->bd_region[i];
      AddMemory((void *)mr->mr_lower, mr->mr_upper - mr->mr_lower, mr->mr_attr);
    }
  }

  SetupExceptionVector(bd);
  SetupInterruptVector();

  /* Set up system interface. */
  memcpy(&ExcVec[EXC_TRAP(16)], JumpTable, (u_int)JumpTableSize);

  /* CIA-A & CIA-B: Stop timers and return to default settings. */
  ciaa->ciacra = 0;
  ciaa->ciacrb = 0;
  ciab->ciacra = 0;
  ciab->ciacrb = 0;

  /* CIA-A & CIA-B: Clear pending interrupts. */
  SampleICR(ciaa, CIAICRF_ALL);
  SampleICR(ciab, CIAICRF_ALL);

  /* CIA-A & CIA-B: Disable all interrupts. */
  WriteICR(ciaa, CIAICRF_ALL);
  WriteICR(ciab, CIAICRF_ALL);

  /* Enable master bit in DMACON and INTENA */
  EnableDMA(DMAF_MASTER);
  EnableINT(INTF_INTEN);

  /* Lower interrupt priority level to nominal. */
  SetIPL(IPL_NONE);

  TaskInit(CurrentTask, "main", bd->bd_stkbot, bd->bd_stksz);
#ifdef TRACKMO
  {
    FileT *dev;
    if (bd->bd_bootdev)
      dev = MemOpen((const void *)0xf80000, 0x80000);
    else
      dev = FloppyOpen();
    InitFileSys(dev);
  }
#endif
  CallFuncList(&__INIT_LIST__);

  {
    int retval = main();
    Log("[Loader] main() returned %d.\n", retval);
  }

  CallFuncList(&__EXIT_LIST__);
#ifdef TRACKMO
  KillFileSys();
#endif
  
  Log("[Loader] Shutdown complete!\n");
}
