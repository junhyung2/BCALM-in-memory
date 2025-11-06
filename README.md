bcalm (In-Memory Version)
=====

de Bruijn CompAction in Low Memory

This is a modified version that uses an in-memory virtual file system (VFS) instead of disk-based temporary files. The algorithm and flow remain identical to the original, but intermediate data (`.bcalmtmp/*` files) are stored in memory for improved performance.

Original Version
=====

The original BCALM (disk-based I/O version) can be found at:
- https://github.com/Malfoy/BCALM (original BCALM repository)
- https://github.com/GATB/bcalm (BCALM2 - newer official version)

This in-memory fork maintains the same algorithm and output format as the original, only changing the I/O mechanism from disk files to memory buffers.

Possible use :

    ./bcalm input
compact input with l=8 in compacted.dot

    ./bcalm input output.dot
compact input with l=8 in output.dot

    ./bcalm input output.dot 10
compact input with l=10 in output.dot



Nota Bene :
Higher l mean lower memory but the algorithm will NOT work with l>10.

In-Memory Version Notes:
- This version does NOT require `ulimit -n` adjustments since it uses an in-memory virtual file system
- No `.bcalmtmp/` directory is created on disk
- All intermediate files are stored in RAM, which may increase memory usage but improves I/O performance
- The algorithm, output format, and behavior are identical to the original disk-based version

Input
=====

Each line in the input file is a distinct k-mer in upper-case, ending with his counting (not required). Here is a sample input:

AATCGATCG 3
ATCGATCGT 33
TCGATCGTA 645

It is the typical ouput of DSK with the dsk2ascii script (see http://minia.genouest.org/dsk/)

Output
=====

Each line is a simple path of the de Bruijn graph, in lowercase. Here is a sample output:

aatcgatcgta;


License
=======

BCALM's license is BSD with Attribution. In informal words, any use is permitted as long as it acknowledges us. For example with a citation to:

    @inproceedings{reprdbg14,
    author = {Chikhi, R. and Limasset, A. and Jackman, S. and Simpson, J. and Medvedev, P.},
    title = {{On the representation of de Bruijn graphs}},
    booktitle = {RECOMB},
    publisher = {Springer},
    year = 2014,
    }

