#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"/*added header*/
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"/*added header*/
#include "threads/synch.h"/*added header*/

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
	char *fn_copy;
	tid_t tid;
	char *name = palloc_get_page(0);
	char *context = NULL;

	/* Make a copy of FILE_NAME.
	   Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	// Make another copy of FILE_NAME
	if (name == NULL)
		return TID_ERROR;
	strlcpy (name, file_name, PGSIZE);
	
	// Extract file name for execution
	name = strtok_r(name, " ", &context);
	

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (name, PRI_DEFAULT, start_process, fn_copy);

	// Free memory
	palloc_free_page(name);
	if (tid == TID_ERROR)
    palloc_free_page (fn_copy); 
  
	return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
	char *file_name = file_name_;
	struct intr_frame if_;
	bool success;

	int count=0;
	char *parse[LOADER_ARGS_LEN / 2 + 1];	//LOADER_ARGS_LEN = 128
	char *token = NULL;
	char *context = NULL;
	
	// file_name is parsed and saved at "parse"
	// which will be passed to the "argument_stack()"
	token = strtok_r(file_name, " ", &context);

	for(; token!=NULL; token=strtok_r(NULL," ",&context)){
		parse[count++] = token;
	}


	/* Initialize HASH TABLE using vm_init() */
	vm_init(&thread_current()->vm);


	/* Initialize interrupt frame and load executable. */
	memset (&if_, 0, sizeof if_);
	if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
	if_.cs = SEL_UCSEG;
	if_.eflags = FLAG_IF | FLAG_MBS;
	success = load (parse[0], &if_.eip, &if_.esp);

	printf("start_process, load success? : %d\n",success);

	// Let parent process know if load of child process 
	// done successfully through "load_flag"
	// Then, wake parent up

	// 1. load successed
	if(success){
		thread_current()->load_flag = true;
		sema_up(&thread_current()->sema_load);
	}

	// 2. load failed, quit
	if (!success) {
		thread_current()->load_flag = false;
		sema_up(&thread_current()->sema_load);
		thread_exit ();
	}

	// Pass argument to USER STACK
	argument_stack(parse, count, &if_.esp);

	hex_dump(if_.esp, if_.esp, PHYS_BASE - if_.esp, true);

	// free memory
	palloc_free_page (file_name);

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
process_wait (tid_t child_tid UNUSED) 
{
	struct thread *child;
	int status;

	// Get child thread through "child_tid"
	child = get_child_process(child_tid);
	if(child == NULL)	return -1;
	
	// Wait child to be exited
	sema_down(&child->sema_wait);

	// Awake from waiting 
	// save status of child and remove child
	status = child->ret_status;
	remove_child_process(child);
	
	return status;
}


/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

	// Close all the open file
	while(cur->fd_next > 2){
		cur->fd_next--;
		process_close_file(cur->fd_next);
	}
	// Close current runnning file
	file_close(cur->running_file);

	//free file_descriptor table
	palloc_free_page(cur->fd_table);

	/* remove vm_entry */
	vm_destroy(&cur->vm);


  /* Destroy the current process's page directory and switch back
	remove_child_process(child);
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
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

	printf("########LOAD PROCESS########\n");

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

	// To avoid access of other file while open
	lock_acquire(&filesys_lock);

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
	  lock_release(&filesys_lock);
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

	// Denying writes to executables
	t->running_file = file;
	file_deny_write(file);
	
	lock_release(&filesys_lock);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
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
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
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

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;
	printf("##################################\n");
 done:
  /* We arrive here whether the load is successful or not. */
  //file_close (file);
  return success;
}


/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

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

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);
	
	printf("load_segment called!\n");

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) 
	{
      	/* Calculate how to fill this page
		   We will read PAGE_READ_BYTES bytes from FILE
		   and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      	size_t page_zero_bytes = PGSIZE - page_read_bytes;
	
		/* Make vm_entry */
		struct vm_entry *vme = malloc(sizeof(struct vm_entry));	
		if(vme == NULL) return false;

		/* Set up members of vm_entry */
		vme->type = VM_BIN;
		vme->vaddr = upage;
		vme->writable = writable;
		vme->is_loaded = false;
		vme->file = file;
		vme->offset = ofs;
		vme->read_bytes = page_read_bytes;
		vme->zero_bytes = page_zero_bytes;
		
		/* Insert vm_entry into Hash Table */
		insert_vme(&thread_current()->vm,vme);


		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE; 
	
	}


	return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  
	uint8_t *kpage;
	bool success = false;

	printf("setup_stack called!\n");

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) 
	{
		success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
		if (success){
			*esp = PHYS_BASE;
		}
		else
			palloc_free_page (kpage);
	}
	
	/* Make vm_entry */
	struct vm_entry *vme = malloc(sizeof(struct vm_entry));	
	if(vme == NULL) return false;

	/* Set up members of vm_entry */
	vme->type = VM_BIN;
	vme->vaddr = (uint8_t*)PHYS_BASE - PGSIZE;
	vme->writable = success;
	vme->is_loaded = success;
	vme->file = NULL;
	vme->offset = 0;
	vme->read_bytes = 0;
	vme->zero_bytes = 0;
	
	/* Insert vm_entry into Hash Table */
	insert_vme(&thread_current()->vm,vme);
		
	printf("setup_stack success ? : %d\n",success);

	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* Pass parsed argument to USER STACK  */
void
argument_stack (char **parse, int count, void **esp)
{
	int i,j;
	int arg_address[count];

	// argv[2],[1],[0]
	for(i=count-1; i>=0; i--){
		for(j=strlen(parse[i]); j>=0; j--){
			*esp = *esp-1;
			**(char **)esp = parse[i][j];
		}
		arg_address[i] = (int) *esp;
	}

	// fit memory in multiple of 4 bytes and put additinal 4 bytes for NULL
	*esp = *esp - 4 - (strlen(parse[0]) + 1) % 4;

	// &argv[2],[1],[0]
	for(i=count-1; i>=0; i--){
		*esp = *esp - 4;
		**(int **) esp = (int)arg_address[i];
	}

	// argv
	*esp = *esp - 4;
	**(int **) esp = (int)(*esp+4);

	// argc
	*esp = *esp - 4;
	**(int **) esp = count;

	// fake address
	*esp = *esp-4;
	**(int **) esp = 0;

}

/* Search child process Descriptor */
struct thread 
*get_child_process (int pid)
{
	struct thread *t;
	struct list *c = &thread_current()->child_list;
	struct list_elem *e;

	for (e=list_begin(c); e!=list_end(c); e=list_next(e)){
		t=list_entry(e, struct thread, child_elem);
		if(t->tid == pid)	return t;
	}	return NULL;
}

/* remove child process */
void
remove_child_process (struct thread* cp)
{
	list_remove (&cp->child_elem);
	palloc_free_page(cp);
}

/* Add file to FD table */
int
process_add_file (struct file *f)
{
	struct thread *t = thread_current();
		
	t->fd_table[t->fd_next] = f;
	return t->fd_next++;
}

/* Get FD table's (fd)th element */
struct file*
process_get_file (int fd)
{
	struct thread *t = thread_current();

	if(t->fd_next <=fd ) return NULL;
	return t->fd_table[fd];
}

/* Closing file */
void
process_close_file (int fd)
{
	struct thread *t = thread_current();
	file_close( t->fd_table[fd] );
	t->fd_table[fd] = NULL;
}

/* Page fault handling */
bool
handle_mm_fault (struct vm_entry *vme)
{

	printf("handle_mm_fault called\n");
	/* Allocate physical memory */
	void *kaddr = palloc_get_page(PAL_USER);
	if(kaddr == NULL) return false;

	/* Handling */
	switch(vme->type){
		case VM_BIN:
			/* Load file to physical page */
			if(load_file(kaddr,vme) == false){
				palloc_free_page(kaddr);
				return false;
			}
			/* Mapping Virtual page on Physical page  */
			if(install_page(vme->vaddr, kaddr, vme->writable) == false){
				palloc_free_page(kaddr);
				return false;
			}
			vme->is_loaded = true;
	
			break;
		case VM_FILE:
			break;
		case VM_ANON:
			break;
		default:
			return false;
	}
	printf("handle_mm_fault, loaded? %d\n",vme->is_loaded);
	return vme->is_loaded;
}


