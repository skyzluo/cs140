#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <list.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devices/input.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#endif

static thread_func start_process NO_RETURN;

/* Info about the process' name and args.  This is only used to pass auxiliary
   data between execute() and start()*/
struct process_info {
  char * prog_name;
  struct file *file;
  char *args_copy;	//pointer to the args data in the heap
  struct semaphore loaded;	//signal when done loading
  bool load_success;	//stores whether it loaded successfully
};

static bool load (struct process_info *pinfo, void (**eip) (void), void **esp);
static void push_args(struct process_info * pinfo, void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  tid_t tid;
  struct process_info *pinfo =  malloc(sizeof(struct process_info));
  if (pinfo == NULL)
    return TID_ERROR;

  memset (pinfo, 0, sizeof (struct process_info));
  sema_init (&pinfo->loaded, 0);
  
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  pinfo->args_copy = palloc_get_page (0);
  if (pinfo->args_copy == NULL) {
    free (pinfo);
    return TID_ERROR;
  }
  strlcpy (pinfo->args_copy, file_name, PGSIZE);

  /* Extract the program name */
  int name_len = strcspn(file_name, " ");
  char *prog_name = calloc(sizeof(char), name_len + 1);
  if (prog_name == NULL) 
  {
    free (pinfo);
    return TID_ERROR;
  }

  memcpy(prog_name, file_name, name_len);
  pinfo->prog_name = prog_name;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (pinfo->prog_name, PRI_DEFAULT, thread_get_cwd (),
                       start_process, pinfo);
  sema_down(&pinfo->loaded);

  /* Loading is now complete */
  if (pinfo->load_success == false) tid = TID_ERROR;
  palloc_free_page (pinfo->args_copy); 
  free(prog_name);
  free(pinfo);

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  struct process_info *pinfo = (struct process_info *) file_name_;
  struct intr_frame if_;
  bool success;

  /* Mark this as a user process */
  thread_current ()->user = true;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* Open the file and prevent writes to it while loading */
  struct file* file = filesys_open (pinfo->prog_name);
  if (file != NULL) file_deny_write (file);
  pinfo->file = file;

  success = load (pinfo, &if_.eip, &if_.esp);

  /* If load failed, allow writes again and quit. */
  if (!success) 
  {
    if (file != NULL) file_allow_write (file);
    thread_current ()->exit_code = -1;
    thread_exit ();
  } else {
    thread_current ()->exec_file = file;
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct list_elem *e;
  struct process_status *pcb = NULL;
  struct thread *cur = thread_current ();
  int status;

  /* Check if tid is one of children */
  for (e = list_begin (&cur->pcb_children); e != list_end
	 (&cur->pcb_children); e = list_next (e))
  {
    pcb = list_entry (e, struct process_status, elem);
    if (pcb->tid == child_tid)
      break;
  }

  /* If not a valid child, or has already been removed, return -1 */
  if (e == list_end (&cur->pcb_children)) return -1;

  /* If still running, wait for a signal */
  lock_acquire (&pcb->l);
  while (pcb->t != NULL)
    cond_wait (&pcb->cond, &pcb->l);

  status = pcb->status;

  /* Clean up the status struct since it is now dead */
  list_remove (&pcb->elem);
  if (pcb->t != NULL)
    pcb->t->pcb = NULL;
  lock_release (&pcb->l);
  free (pcb);

  return status;
}

/* Creates a PCB for a thread and initialize relevant fields */
bool
process_create_pcb (struct thread *t)
{
  /* Allocate object */
  t->pcb = malloc (sizeof (struct process_status));
  if (t->pcb == NULL) return false;

  /* Initialize object */
  t->pcb->tid = t->tid;
  t->pcb->t = t;
  lock_init (&t->pcb->l);
  cond_init (&t->pcb->cond);
  list_init (&t->fd_list);
  t->next_fd = PFD_OFFSET;
  t->exec_file = NULL;

   /* Initialize list of child processes */
  list_init (&t->pcb_children);

  /* Link new thread's PCB up to its parent thread */
  list_push_back (&thread_current ()->pcb_children, &t->pcb->elem);

  return true;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Print exit message */
  if (cur->user)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_code);

  /* Close files that the process holds */
  struct list *fds = &cur->fd_list;
  while (!list_empty (fds))
  {
    struct list_elem *e = list_front (fds);
    struct process_fd * fd = list_entry (e, struct process_fd, elem);

    process_mmap_file_close (fd->file);

    syscall_close (fd->fd);
  }

  /* Interact with our pcb object */
  if (cur->pcb != NULL)
  {
    lock_acquire (&cur->pcb->l);
    cur->pcb->status = cur->exit_code;
    cur->pcb->t = NULL;
    cond_signal (&cur->pcb->cond, &cur->pcb->l);
    lock_release (&cur->pcb->l);
  }

  /* Kill all the remaining child pcb objects */
  struct list *children = &cur->pcb_children;
  while (!list_empty (children))
  {
    struct process_status *pcb = 
      list_entry (list_pop_front (children), 
                  struct process_status, elem);
    lock_acquire (&pcb->l); 
    if (pcb->t != NULL)
      pcb->t->pcb = NULL;
    lock_release (&pcb->l);
    free (pcb);
  }

  /* Allow writes to the exec file again */
  if (cur->exec_file != NULL) 
  {
    file_allow_write (cur->exec_file);
    file_close (cur->exec_file);
  }

#ifdef VM

  /* Unallocate all remaining pages in the supplemental page table */
  lock_acquire (&cur->s_page_lock);
  hash_destroy (&cur->s_page_table, page_destroy_thread);
  lock_release (&cur->s_page_lock);
#endif

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static void stack_push (void ** esp, void * data, size_t size);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);

static bool
load_segment (struct process_info *pinfo, uint32_t file_page,
	      uint8_t *base_uaddr, size_t read_bytes,
	      size_t zero_bytes, bool writable) 
{
  uint8_t *uaddr = base_uaddr;

  while (read_bytes > 0 || zero_bytes > 0)
  {
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    bool success;
    if (page_read_bytes > 0)
    {
      if (writable) 
        success = vm_add_file_init_page (uaddr, pinfo->file, file_page,
					 page_zero_bytes);
      else
        success = vm_add_file_page (uaddr, pinfo->file, file_page,
				    page_zero_bytes, false);
    }
    else 
    {
      success = vm_add_memory_page (uaddr, writable);
    }
    if (!success)
      return false;

    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    uaddr += PGSIZE;
    file_page += page_read_bytes;
  }

  return true;
}

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (struct process_info *pinfo, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = pinfo->file;
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", pinfo->prog_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", pinfo->prog_name);
      goto done; 
    }
  
  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
	  {
        goto done;
	  }
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
	  {
        goto done;
	  }
      file_ofs += sizeof phdr;

      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
              {
                /* Normal segment. Will be read from disk */
                read_bytes = page_offset + phdr.p_filesz;
                zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz,
                      PGSIZE) - read_bytes);
              } else {
                /* Entirely zero. Don't read anything from disk. */
                read_bytes = 0;
                zero_bytes = ROUND_UP (page_offset + phdr.p_memsz,
                    PGSIZE);
              }
              if (!load_segment (pinfo, file_page, (void*)mem_page,
                    read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;
  /* Push arg info onto the stack */
  push_args(pinfo, esp);

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  pinfo->load_success = success;
  sema_up(&pinfo->loaded);
  return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Push arguments, their references, and argc onto the stack */
static void 
push_args(struct process_info *pinfo, void **esp) {

  /* Null-terminate the strings and count the arguments */
  int argc = 0;
  char *token, *save_ptr;
  for (token = strtok_r (pinfo->args_copy, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr)) {
    argc++;
  }

  /* Array of pointers to the strings on the stack */
  char *argv[argc];

  /* Push the argument strings to the stack */
  char *str_ptr = pinfo->args_copy;
  int i;
  for (i = 0; i < argc; i++) {	
    stack_push(esp, str_ptr, strlen(str_ptr) + 1);
    str_ptr = strchr(str_ptr, '\0') + 1;
    /* skip all delimiters */
    while (*str_ptr == ' ') str_ptr++;
    /* Save the stack location */
    argv[i] = *esp;
  }
  /* pad to 4-bytes */
  for (i = 0; i < ((int)(*esp) % 4); i++) {
    char c = 0;
    stack_push(esp, &c, sizeof(c));
  }
  /* Push the arg pointers, in reverse order */
  /* Start with a null ptr for args[argc] */
  int zero = 0;
  stack_push(esp, &zero, sizeof(zero));	
  for (i = argc - 1; i >= 0; i--)
    stack_push(esp, &argv[i], sizeof(char*));

  /* Push the address of the first arg pointer */
  void *saved_esp = *esp;
  stack_push(esp, &saved_esp, sizeof(void*));
  /* Push argc */
  stack_push(esp, &(argc), sizeof(argc));
  /* Push the return address */
  stack_push(esp, &zero, sizeof(zero));
}

/* Push an element of size 'size' onto a stack.  This will check
   the page boundary */
static void
stack_push (void **esp, void *data, size_t size)
{
  *esp -= size;
  memcpy(*esp, data, size);
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  bool success = vm_add_memory_page (((uint8_t*)PHYS_BASE) - PGSIZE, true);
  if (success)
    *esp = PHYS_BASE;
  return success;
}

static struct process_fd*
get_process_fd (struct thread *t, int fd) 
{
  if (fd < PFD_OFFSET) return NULL;

  struct list *fd_list = &t->fd_list;
  struct list_elem *elem = list_begin (fd_list);
  for (; elem != list_end (fd_list); elem = list_next (elem))
  {
    struct process_fd *pfd = 
      list_entry (elem, struct process_fd, elem);

    if (pfd->fd == fd) return pfd;
  }

  return NULL;
}

/* this function is not responsible for freeing filename but expects
   it to be valid memory while the process fd is still in the list */
int 
process_add_file (struct thread *t, struct file *file, 
                  const char* filename)
{
  struct list *fd_list = &t->fd_list;

  struct process_fd *new_fd = malloc (sizeof (struct process_fd));
  if (new_fd == NULL) return -1;
  new_fd->file = file;
  new_fd->fd = t->next_fd++;
  new_fd->filename = strdup (filename);

  if (new_fd->filename == NULL) 
  {
    free (new_fd);
    return -1;
  }

  list_push_back (fd_list, &new_fd->elem);
  return new_fd->fd;
}

struct process_fd* 
process_get_file (struct thread *t, int fd) 
{
  struct process_fd* pfd = get_process_fd (t, fd);
  return pfd;
}

void
process_remove_file (struct thread *t, int fd) 
{
  struct process_fd* pfd = get_process_fd (t, fd);

  if (pfd == NULL) return;
  list_remove (&pfd->elem);
  free (pfd->filename);
  free (pfd);
}

struct process_mmap* 
mmap_create (const char *filename)
{
  ASSERT (filename != NULL);
  struct process_mmap *mmap = malloc (sizeof (struct process_mmap));
  if (mmap == NULL)
    return NULL;

  /* Make a copy of the file struct. */
  int fd = syscall_open (filename);
  struct process_fd *pfd = process_get_file (thread_current (), fd);
  struct file * file = pfd->file;
  if (file == NULL) return NULL;

  list_init (&mmap->entries);
  mmap->size = file_length (file);
  mmap->file = file;
  mmap->id = INVALID_MMAP_ID;

  return mmap;
}

bool mmap_add (struct process_mmap *mmap, void* uaddr, 
                   unsigned offset)
{
  /* Check that there is no existing mapping for the current thread
     for this address */
  struct thread *t = thread_current();

  if (pagedir_get_page (t->pagedir, uaddr)) return false;

  /* Also chheck the supplemental page table */
  struct s_page_entry key = {.uaddr = uaddr};

  lock_acquire (&t->s_page_lock);
  struct hash_elem *e = hash_find (&t->s_page_table, &key.elem);
  if (e != NULL) 
  {
    lock_release (&t->s_page_lock);
    return false;
  }
  lock_release (&t->s_page_lock);

  /* Check if there are zero bytes on this page */
  uint32_t zero_bytes = 0;
  uint32_t file_remain = mmap->size - offset;

  if (file_remain < PGSIZE) 
    zero_bytes = PGSIZE - file_remain;
 
  struct mmap_entry *entry = malloc (sizeof (struct mmap_entry));
  if (entry == NULL) return false;


  entry->uaddr = uaddr;
  bool success = vm_add_file_page (uaddr, mmap->file, offset,
				   zero_bytes, true);
  if (!success) 
  {
    free (entry);
    return false;
  }

  list_push_back (&mmap->entries, &entry->elem);

  return true;
}

/* Frees the memmory associated with an mmap and unmaps its pages
   from memory */
void mmap_destroy (struct process_mmap *mmap)
{
  /* Unmap each of the entries in the entire map */
  struct thread *t = thread_current ();

  struct list_elem *e = NULL; 
  struct list_elem *next_e = NULL;
  for (e = list_begin (&mmap->entries); 
        e != list_end (&mmap->entries); e = next_e)
  {
    next_e = list_next (e);
    struct mmap_entry *entry = list_entry (e, struct mmap_entry, elem);

    lock_acquire (&t->s_page_lock);
    struct s_page_entry key = {.uaddr = entry->uaddr};
    struct hash_elem *e = hash_find (&t->s_page_table, &key.elem);
    if (e == NULL) 
    {
      lock_release (&t->s_page_lock);
      PANIC ("Error in mmap code, missing page entry");
    }

    /* Lock on this supplemental page entry */
    struct s_page_entry *spe = hash_entry (e, struct s_page_entry, elem);
    lock_release (&t->s_page_lock);
    vm_free_page (spe);
    free (entry);
  }

  free (mmap);
}

/* Adds the mmap to the current process and returns its id */
int process_add_mmap (struct process_mmap *mmap)
{
  struct thread *t = thread_current ();
  mmap->id = t->next_mmap++;

  list_push_back (&t->mmap_list, &mmap->elem);

  return mmap->id;
}

/* Finds the mmap with the given id */
static struct process_mmap *
process_get_mmap (int id) 
{
  struct process_mmap *result = NULL;

  struct thread *t = thread_current ();

  struct list_elem *e = NULL; 
  for (e = list_begin (&t->mmap_list); 
        e != list_end (&t->mmap_list); e = list_next(e))
  {
    struct process_mmap *entry = list_entry 
                                (e, struct process_mmap, elem);
    if (entry->id == id) 
    {
      result = entry;
      break;
    }
  }

  return result;
}

/* Helper for cleaning up a an mmapping from its process. */
static bool
process_kill_mmap (struct process_mmap *mmap) 
{
  if (mmap == NULL) return false;
  list_remove (&mmap->elem);
  mmap_destroy (mmap);
  
  return true;
}

bool
process_remove_mmap (int id)
{
  struct process_mmap *mmap = process_get_mmap (id);
  return process_kill_mmap (mmap);
}

/* Loops through mmaps, killing all that use the same file as a 
   target */
void process_mmap_file_close (struct file* file)
{
  struct thread *t = thread_current ();
  struct list_elem *e = NULL; 
  struct list_elem *e_next = NULL;
  for (e = list_begin (&t->mmap_list); 
        e != list_end (&t->mmap_list); e = e_next)
  {
    e_next = list_next(e);
    struct process_mmap *entry = list_entry 
                                (e, struct process_mmap, elem);
    if (entry->file == file) 
      process_kill_mmap (entry);
  }
}
