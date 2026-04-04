#include "types.h"
#include "x86.h"
#include "defs.h"
//#include "date.h"
#include "param.h"
//#include "memlayout.h"
//#include "mmu.h"

int
sys_countvp(void)
{
  return countvp();
}


int
sys_countpp(void)
{
  return countpp();
}


int
sys_countptp(void)
{
  return countptp();
}


int
sys_countfp(void)
{
  return countfp();
}