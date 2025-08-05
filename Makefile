MPICC ?= mpicc
CFLAGS ?= -O3
LDFLAGS ?=
SHMEM_CC ?= oshcc

all: mpi_overhead msgrate rma_mt_mpi shmem_mt

mpi_overhead: src/mpi_overhead/mpi_overhead.c
	$(MPICC) $(CFLAGS) -o $@ $< $(LDFLAGS)

msgrate: src/msgrate/msgrate.c
	$(MPICC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rma_mt_mpi: src/rma_mt_mpi/msgrate.c
	$(MPICC) $(CFLAGS) -o $@ $< -pthread $(LDFLAGS)

shmem_mt: src/shmem_mt/msgrate.c
	$(SHMEM_CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f mpi_overhead msgrate rma_mt_mpi shmem_mt

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL_TARGETS ?= all

install: $(INSTALL_TARGETS)
	mkdir -p $(BINDIR)
	install -m 755 mpi_overhead $(BINDIR)
	install -m 755 msgrate $(BINDIR)
	install -m 755 rma_mt_mpi $(BINDIR)
	install -m 755 shmem_mt $(BINDIR)

uninstall:
	rm -f $(BINDIR)/mpi_overhead
	rm -f $(BINDIR)/msgrate
	rm -f $(BINDIR)/rma_mt_mpi
	rm -f $(BINDIR)/shmem_mt

.PHONY: all clean install uninstall dist