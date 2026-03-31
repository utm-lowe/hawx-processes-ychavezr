#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "mem.h"
#include "string.h"
#include "console.h"
#include "trap.h"
#include "proc.h"
#include "scheduler.h"

// Extern Globals
extern pagetable_t kernel_pagetable; // mem.c
extern char trampoline[];            // trampoline.S
extern char _binary_user_init_start; // The user init code
extern void swtch(struct context *, struct context *);

////////////////////////////////////////////////////////////////////////////////
// Static Definitions and Helper Function Prototypes
////////////////////////////////////////////////////////////////////////////////
static int nextpid = 1;
static pagetable_t proc_pagetable(struct proc *);
static void proc_free_pagetable(pagetable_t pagetable, uint64 sz);
static void proc_freewalk(pagetable_t pagetable);
static uint64 proc_shrink(pagetable_t pagetable, uint64 oldsz, uint64 newsz);
static int proc_loadseg(pagetable_t pagetable, uint64 va, void *bin, uint offset, uint sz);
static void proc_guard(pagetable_t pagetable, uint64 va);

////////////////////////////////////////////////////////////////////////////////
// Global Definitions
////////////////////////////////////////////////////////////////////////////////
struct cpu cpu;
struct proc proc[NPROC];

////////////////////////////////////////////////////////////////////////////////
// Process API Functions
////////////////////////////////////////////////////////////////////////////////

// Initialize the proc table, and allocate a page for each process's
// kernel stack. Map the stacks in high memory, followed by an invalid guard page.
void proc_init(void)
{
  // You need to loop over all the proc structs and set up their stacks
  // This setup requires two steps:
  //   1.) Use the KSTACK macro to set up the kstack field in the struct
  //   2.) Allocate a new physical page for the stack and insert it
  //       into the kernel's page table at the virtual address referred
  //       to by kstack.
  // HINTS: This function is a combination of two functions in xv6.
  //        I used the following memory functions:
  //           vm_page_alloc
  //           vm_page_insert
  // YOUR CODE HERE
  for (int i = 0; i < NPROC; i++)
  {
    proc[i].state = UNUSED;
    proc[i].pid = 0;
    proc[i].wait_read = 0;
    proc[i].wait_write = 0;
    proc[i].sz = 0;
    proc[i].pagetable = 0;
    proc[i].trapframe = 0;
    memset(&proc[i].context, 0, sizeof(proc[i].context));
    proc[i].kstack = KSTACK(i);

    void *pa = vm_page_alloc();
    if (pa == 0)
      panic("proc_init");

    if (vm_page_insert(kernel_pagetable, proc[i].kstack, (uint64)pa, PTE_R | PTE_W) != 0)
      panic("proc_init");
  }
}

// Set up the first user process. Return the process it was allocated to.
struct proc *
proc_load_user_init(void)
{
  void *bin = &_binary_user_init_start;
  struct proc *p = 0x00;

  // Allocate a new process. If there is no process avaialble, panic.
  // Use proc_load_elf to load up the elf string.
  // As an additional hint, I have defined the variables you need
  // for you. The bin pointer points to the embedded BLOB which
  // contains the program image for init.
  // YOUR CODE HERE
  p = proc_alloc();
  if (p == 0)
    panic("proc_load)_user_init");

  if (proc_load_elf(p, bin) < 0)
    panic("proc_load_user_init");

  return p;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *
proc_alloc(void)
{
  // Search for an unused process in the proc array. If you do
  // not find one, return 0. If you do find one, do the following:
  //   1.) Set the pid field to the next available pid.
  //       (be sure to update nextpid)
  //   2.) Allocate a trapframe page for the proces.
  //   3.) Set the trapframe page to all zeroes.
  //   4.) Allocate an empty page table for the process.
  //   5.) Set the return address in the processe's context to
  //       return to usertrapret.
  //   6.) Set the process stack pointer to one address past the end of the
  //       kstack page.
  // HINTS: This function combines several ideas from xv6 function, but it
  //        does require adaptation.
  //        I used the following functions:
  //          vm_pagealloc
  //          proc_free
  //          memset
  //          proc_pagetable
  // YOUR CODE HERE
  for (int i = 0; i < NPROC; i++)
  {
    struct proc *p = &proc[i];

    if (p->state == UNUSED)
    {
      p->state = USED;

      p->wait_read = 0;
      p->wait_write = 0;

      p->pid = nextpid;
      nextpid++;

      p->trapframe = (struct trapframe *)vm_page_alloc();
      if (p->trapframe == 0)
      {
        proc_free(p);
        return 0;
      }
      memset(p->trapframe, 0, PGSIZE);

      p->pagetable = proc_pagetable(p);
      if (p->pagetable == 0)
      {
        proc_free(p);
        return 0;
      }

      memset(&p->context, 0, sizeof(p->context));
      p->context.ra = (uint64)usertrapret;
      p->context.sp = p->kstack + PGSIZE;

      p->sz = 0;

      return p;
    }
  }
  return 0;
}

// free a proc structure and the data hanging from it,
// including user pages.
void proc_free(struct proc *p)
{
  // Free the process's trapframe, empty its pagetable,
  // and reset all fields to zero. The state of the process
  // should be "UNUSED".
  // HINT: Functions I used
  //         vm_page_free
  //         proc_free_pagetable
  // YOUR CODE HERE
  if (p->trapframe)
    vm_page_free((void *)p->trapframe);
  p->trapframe = 0;

  if (p->pagetable)
    proc_free_pagetable(p->pagetable, p->sz);
  p->pagetable = 0;

  p->state = UNUSED;
  p->wait_read = 0;
  p->wait_write = 0;
  p->pid = 0;
  p->sz = 0;
  memset(&p->context, 0, sizeof(p->context));
}

// Load the ELF program image stored in the binary string bin
// into the specified process. This operation will destroy the
// pagetable currently in p, and replace it with a page table
// as indicated by the segments of the elf formatted binary.
int proc_load_elf(struct proc *p, void *bin)
{
  struct elfhdr elf;
  struct proghdr ph;
  int i, off;
  uint64 sz = 0, sp = 0;
  pagetable_t pagetable = 0;

  // get the elf header from bin
  elf = *(struct elfhdr *)bin;

  // check the elf magic
  if (elf.magic != ELF_MAGIC)
    goto bad;

  // We need to load the process from the binary string pointed to
  // by bin. This is similar to xv6's exec function, but with
  // several key differences:
  //   - We are loading from bin, not from the disk.
  //   - Offsets give the number of bytes from the beginning of bin.
  //   - We will not be putting program arguments on the stack.
  // The basic steps we need to perform are as follows:
  //   1.) Create a new pagetable for the process
  //   2.) Loop over all of the program headers in the elf object
  //       - Check the validity of the header, goto bad if invalid
  //       - Use proc_resize to increase the size of the process to
  //         hold this segment's data
  //       - Use proc_loadseg to load this segment into user memory
  //   3.) Set up the user stack
  //   4.) Destroy the old page table
  //   5.) Commit to the user image
  //   6.) Mark the process as runnable
  //   7.) Return 0
  //
  // If we have bad elf image, do the following:
  //   1.) Free the page table if it exists
  //   2.) return -1
  // HINT: The key to this function is to fully understand how the
  //       elf object looks in the binary object. I have given you
  //       my variables as well as how you obtain the elf header
  //       as a hint. You will also need to fully understand how
  //       exec works in xv6. Happy reading!
  // YOUR CODE HERE
  pagetable_t oldpagetable;
  uint64 oldsz;
  pagetable = proc_pagetable(p);
  if (pagetable == 0)
    goto bad;

  off = elf.phoff;
  for (i = 0; i < elf.phnum; i++, off += sizeof(ph))
  {
    ph = *(struct proghdr *)((char *)bin + off);

    if (ph.type != ELF_PROG_LOAD)
      continue;

    if (ph.memsz < ph.filesz)
      goto bad;

    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;

    if (ph.vaddr % PGSIZE != 0)
      goto bad;

    sz = proc_resize(pagetable, sz, ph.vaddr + ph.memsz);
    if (sz == 0)
      goto bad;

    if (proc_loadseg(pagetable, ph.vaddr, bin, ph.off, ph.filesz) < 0)
      goto bad;

    int perm = PTE_U | PTE_R;
    if (ph.flags & ELF_PROG_FLAG_WRITE)
      perm |= PTE_W;
    if (ph.flags & ELF_PROG_FLAG_EXEC)
      perm |= PTE_X;

    for (uint64 a = ph.vaddr; a < ph.vaddr + ph.memsz; a += PGSIZE)
    {
      pte_t *pte = walk_pgtable(pagetable, a, 0);
      if (pte == 0 || (*pte & PTE_V) == 0)
        goto bad;

      *pte = PA2PTE(PTE2PA(*pte)) | perm | PTE_V;
    }
  }


  sz = PGROUNDUP(sz);
  sz = proc_resize(pagetable, sz, sz + 2 * PGSIZE);
  if (sz == 0)
    goto bad;

  proc_guard(pagetable, sz - 2 * PGSIZE);
  sp = sz;
  sp -= sp % 16;


  oldpagetable = p->pagetable;
  oldsz = p->sz;


  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;
  p->trapframe->sp = sp;

  if (oldpagetable)
    proc_free_pagetable(oldpagetable, oldsz);

 
  p->state = RUNNABLE;

  return 0;
bad:
  // YOUR CODE HERE
  if (pagetable)
    proc_free_pagetable(pagetable, sz);
  return -1;
}

// Resize the process so that it occupies newsz bytes of memory.
// If newsz > oldsz
//   Allocate PTEs and physical memory to grow process from oldsz to
// If newsz < oldsz
//   Use proc_shrink to decrease the zie of the process.
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 proc_resize(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  // Make this behave as above. This is a little bit different from the
  // xv6 equivalent. What did I change?
  //
  // YOUR CODE HERE
  if (newsz < oldsz)
    return proc_shrink(pagetable, oldsz, newsz);

  if (newsz > oldsz)
  {
    uint64 a = PGROUNDUP(oldsz);
    if (a < newsz)
    {
      if (vm_map_range(pagetable, a, newsz - a, PTE_R | PTE_W | PTE_U) != 0)
        return 0;
    }
  }

  return newsz;
}

// Given a parent process's page table, copy its memory into a
// child's page table. Copies both the page table and the physical
// memory.
int proc_vmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  // This function is pretty close to the xv6 version, though we use
  // the functions we wrote in mem.c. (See the mem.h file for the
  // documetnation on these functions.) What this function does is
  // the following:
  // 1.) It goes through every page in the old processes page table.
  // 2.) It allocates new memory for the new process page table.
  // 3.) It copies the memory from the old process to the new process.
  // 4.) It maps this new memory to the new process.
  // You should also make sure to handle errors as was done in the xv6
  // table.
  // YOUR CODE HERE
  uint64 pa, i;
  pte_t *pte;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE)
  {
    pte = walk_pgtable(old, i, 0);
    if (pte == 0)
      continue;
    if ((*pte & PTE_V) == 0)
      continue;

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    mem = vm_page_alloc();
    if (mem == 0)
    {
      vm_page_remove(new, 0, i / PGSIZE, 1);
      return -1;
    }

    memmove(mem, (char *)pa, PGSIZE);

    if (vm_page_insert(new, i, (uint64)mem, flags) != 0)
    {
      vm_page_free(mem);
      vm_page_remove(new, 0, i / PGSIZE, 1);
      return -1;
    }
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Static Helper Functions
////////////////////////////////////////////////////////////////////////////////

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
static pagetable_t
proc_pagetable(struct proc *p)
{
  // Create a new pagetable for the process. Do not assign it yet, just
  // return the pagetable after you create it.
  // The page table should contain the following entries:
  //   - Map the trampoline physical address to the TRAMPOLINE virtual address.
  //     trampoline should be readable and executable.
  //   - Map the p->trapframe physical address to the TRAPFRAME virtual address.
  //     The trapframe page should be readable and writable.
  // The functions I used here were:
  //    vm_create_pagetable
  //    vm_page_insert
  //    vm_page_free
  //    vm_page_remove
  // YOUR CODE HERE
  pagetable_t pagetable;

  pagetable = vm_create_pagetable();
  if (pagetable == 0)
    return 0;

  if (vm_page_insert(pagetable, TRAMPOLINE, (uint64)trampoline, PTE_R | PTE_X) != 0)
  {
    vm_page_free((void *)pagetable);
    return 0;
  }

  if (vm_page_insert(pagetable, TRAPFRAME, (uint64)p->trapframe, PTE_R | PTE_W) != 0)
  {
    vm_page_remove(pagetable, TRAMPOLINE, 1, 0);
    vm_page_free((void *)pagetable);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
static void
proc_free_pagetable(pagetable_t pagetable, uint64 sz)
{
  // 1.) Remove the TRAMPOLINE and TRAPFRAME pages
  // 2.) Remove all the user memory pages, freeing their
  //    physical memory.
  // 3.) Free the user page table.
  // Functions Used: vm_page_remove, proc_freewalk
  // YOUR CODE HERE
  vm_page_remove(pagetable, TRAMPOLINE, 1, 0);

  vm_page_remove(pagetable, TRAPFRAME, 1, 0);

  if (sz > 0)
    vm_page_remove(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);

  proc_freewalk(pagetable);
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
static void
proc_freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      proc_freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  vm_page_free((void *)pagetable);
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
static uint64
proc_shrink(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    vm_page_remove(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
proc_loadseg(pagetable_t pagetable, uint64 va, void *bin, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  // Load the program segment from the bin array. Note that
  // offset could be thought of as the offset into bin. There
  // is an equivalent xv6 function which does this using inode
  // loading. The secret to converting it is you are going to
  // have a line that uses memmove and the following expression:
  //   bin+offset+i
  // As an added hint, I have included my variable declarations
  // above.
  // YOUR CODE HERE

  for (i = 0; i < sz; i += PGSIZE)
  {
    pa = vm_lookup(pagetable, va + i);
    if (pa == 0)
      return -1;

    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;

    memmove((void *)pa, (char *)bin + offset + i, n);
  }
  return 0;
}

// mark a PTE invalid for user access.
// used by proc_load_elf for the user stack guard page.
static void
proc_guard(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk_pgtable(pagetable, va, 0);
  if (pte == 0)
    panic("proc_guard");
  *pte &= ~PTE_U;
}

// Find the process with the given pid and return a pointer to it.
// If the process is not found, return 0
struct proc *proc_find(int pid)
{
  // Simply search the proc array, looking for the specified pid.
  // YOUR CODE HERE
  for (int i = 0; i < NPROC; i++)
  {
    if (proc[i].pid == pid)
    {
      return &proc[i];
    }
  }
  return 0;
}

void scheduler(void)
{
  struct proc *p;

  cpu.proc = 0;
  for (;;)
  {
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
      {
        p->state = RUNNING;
        cpu.proc = p;
        swtch(&cpu.context, &p->context);
        cpu.proc = 0;
      }
    }
  }
}

void yield(void)
{
  struct proc *p = cpu.proc;

  if (p == 0)
    return;

  if (p->state == RUNNING)
    p->state = RUNNABLE;

  cpu.proc = 0;
  swtch(&p->context, &cpu.context);
}