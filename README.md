## <a name="start"></a>Getting Started
```sh
# Compile
git clone https://github.com/lh3/ropebwt3
cd ropebwt3
make  # use "make omp=0" if your compiler doesn't suport OpenMP

# Toy examples
echo -e 'AGG\nAGC' | ./ropebwt3 build -LR -
echo TGAACTCTACACAACATATTTTGTCACCAAG | ./ropebwt3 build -Ldo idx.fmd -
echo ACTCTACACAAgATATTTTGTCA | ./ropebwt3 mem -Ll10 idx.fmd -

# Download the prebuilt FM-index for 152 M. tuberculosis genomes
wget -O- https://zenodo.org/records/12803206/files/mtb152.tar.gz?download=1 | tar -zxf -

# Count super-maximal exact matches (no contig positions)
echo ACCTACAACACCGGTGGCTACAACGTGG  | ./ropebwt3 mem -L mtb152.fmd -
# Local alignment
echo ACCTACAACACCGGTaGGCTACAACGTGG | ./ropebwt3 sw -Lm20 mtb152.fmd -
# Retrieve R15311, the 46th genome in the collection. 90=(46-1)*2
./ropebwt3 get mtb152.fmd 90 > R15311.fa

# Download human index
wget -O human100.fmr.gz https://zenodo.org/records/13147120/files/human100.fmr.gz?download=1
wget -O human100.fmd.ssa https://zenodo.org/records/13147120/files/human100.fmd.ssa?download=1
wget -O human100.fmd.len.gz https://zenodo.org/records/13147120/files/human100.fmd.len.gz?download=1
gzip -d human100.fmr.gz
./ropebwt3 build -i human100.fmr -do human100.fmd   # convert the static format for speed

# Find C4 alleles (the query is on the exon 26 of C4A)
echo CCAGGACCCCTGTCCAGTGTTAGACAGGAGCATGCAG | ./ropebwt3 sw -eN200 -Lm10 human100.fmd -
```

## Table of Contents

- [Getting Started](#start)
- [Introduction](#intro)
- [Usage](#use)
  - [Counting maximal exact matches](#mem)
  - [Local alignment](#bwasw)
  - [Haplotype diversity with end-to-end alignment](#e2e)
  - [Constructing a BWT](#build)
  - [Binary BWT formats](#format)
- [Limitations](#limit)

## <a name="intro"></a>Introduction

Ropebwt3 constructs the FM-index of a large DNA sequence set and searches for
matches against the FM-index. It is optimized for highly redundant sequence
sets such as a pangenome or sequence reads at high coverage. Ropebwt3 can
losslessly compress 7.3Tb of common bacterial genomes into a 30GB run-length
encoded BWT file and report supermaximal exact matches (SMEMs) or local
alignments with mismatches and gaps.

Prebuilt ropebwt3 indices can be downloaded [from Zenodo][zenodo].

## <a name="use"></a>Usage

A full ropebwt3 index consists of three files:

* `<base>.fmd`: run-length encoded BWT that supports the rank operation. It is
  generated by the `build` command. By default, the $i$-th sequence in the input
  is the $2i$-th sequence in the BWT and its reverse complement is the
  $(2i+1)$-th sequence. Some commands assume such ordering.

* `<base>.fmd.ssa`: sampled suffix array, generated by the `ssa` command. For
  now, it is only needed for reporting coordinates in the PAF output of the
  `sw` command.

* `<base>.fmd.len.gz`: list of sequence names and lengths. It is generated
  with third-party tools/scripts, for example, with `seqtk comp input.fa | cut
  -f1,2 | gzip`. This file is needed for reporting sequence names and lengths
  in the PAF output.

### <a name="mem"></a>Counting maximal exact matches

A maximal exact match (MEM) is an exact alignment between the index and a query
that cannot be extended in either direction. A super MEM (SMEM) is a MEM that
is not contained in any other MEM on the query sequence. You can find the SMEMs
with
```sh
ropebwt3 mem -t4 -l31 bwt.fmd query.fa > matches.bed
```
In the output, the first three columns give the query sequence name, start and
end of a match and the fourth column gives the number of hits. Option `-l`
specifies the minimum SMEM length. A larger value helps performance.

You can use `--gap` to obtain regions not covered by long SMEMs or `--cov` to
get the total length of regions covered by long SMEMs.

### <a name="bwasw"></a>Local alignment

Ropebwt3 implements a revised [BWA-SW algorithm][bwasw] to align query
sequences against an FM-index:
```sh
ropebwt3 sw -t4 -N25 -k11 bwt.fmd query.fa > aln.paf
```
Option `-N` effectively sets the bandwidth during alignment. A larger value
improves alignment accuracy at the cost of performance. Option `-k` initiates
alignments with an exact match.

Given a complete ropebwt3 index with sampled suffix array and sequence names,
the `sw` command outputs standard PAF but it only outputs one hit per query
even if there are multiple equally best hits. The number of hits in BWT is
written to the `rh` tag.

**Local alignment is tens of times slower than finding SMEMs.** It is not designed
for aligning high-throughput sequence reads.

### <a name="e2e"></a>Haplotype diversity with end-to-end alignment

With option `-e`, the `sw` command aligns the query sequence from end to end.
In this mode, ropebwt3 may output multiple suboptimal end-to-end hits.
This provides a way to retrieve similar haplotypes from the index.

The `hapdiv` command applies this algorithm to 101-mers in a query sequence and
outputs 1) query name, 2) query start, 3) query end, 4) number of distinct
alleles the 101-mer matches, 5) number of haplotypes with perfectly matching
the 101-mer, 6) number of haplotypes with edit distance 1 from the 101-mer,
7) with distance 2, 8) with distance 3 and 9) with distance 4 or higher.

### <a name="build"></a>Constructing a BWT

Ropebwt3 implements three algorithms for BWT construction. For the best
performance, you need to choose an algorithm based on the input date types.

```sh
# If not sure, use the general command line
ropebwt3 build -t24 -bo bwt.fmr file1.fa file2.fa filen.fa
# You can also append another file to an existing index
ropebwt3 build -t24 -i bwt-old.fmr -bo bwt-new.fmr filex.fa
# If each file is small, concatenate them together
cat file1.fa file2.fa filen.fa | ropebwt3 build -t24 -m2g -bo bwt.fmr -
# For short reads, use the old ropebwt2 algorithm and optionally apply RCLO (option -r)
ropebwt3 build -r -bo bwt.fmr reads.fq.gz
# use grlBWT, which may be faster but uses working disk space
ropebwt3 fa2line genome1.fa genome2.fa genomen.fa > all.txt
grlbwt-cli all.txt -t 32 -T . -o bwt.grl
grl2plain bwt.rl_bwt bwt.txt
ropebwt3 plain2fmd -o bwt.fmd bwt.txt
```

These command lines construct a BWT for both strands of the input sequences.
You can skip the reverse strand by adding option `-R`.
If you provide multiple files on a `build` command line, ropebwt3 internally
will run `build` on each input file and then incrementally merge each
individual BWT to the final BWT.

### <a name="format"></a>Binary BWT file formats

Ropebwt3 uses two binary formats to store run-length encoded BWTs: the ropebwt2
FMR format and the fermi FMD format. The FMR format is dynamic in that you can
add new sequences or merge BWTs to an existing FMR file. The same BWT does not
necessarily lead to the same FMR. The FMD format is simpler in structure,
faster to load, smaller in memory and can be memory-mapped. The two formats can
often be used interchangeably in ropebwt3, but it is recommended to use FMR for BWT
construction and FMD for finding exact matches. You can explicitly convert
between the two formats with:
```sh
ropebwt3 build -i in.fmd -bo out.fmr  # from static to dynamic format
ropebwt3 build -i in.fmr -do out.fmd  # from dynamic to static format
```
<!--
## <a name="dev"></a>For Developers

You can encode and decode a FMD file with [rld0.h](rld0.h) and
[rld0.c](rld0.c). The two-file library also supports the rank() operator. Here
is a small program to convert FMD to plain text:
```c
// compile with "gcc -O3 rld0.c this.c"; run with "./a.out idx.fmd > out.txt"
#include <stdio.h>
#include "rld0.h"
int main(int argc, char *argv[]) {
  if (argc < 2) return 1;
  rld_t *e = rld_restore(argv[1]);
  rlditr_t ei; // iterator
  rld_itr_init(e, &ei, 0);
  int c;
  int64_t i, l;
  while ((l = rld_dec(e, &ei, &c, 0)) > 0)
    for (i = 0; i < l; ++i) putchar("\nACGTN"[c]);
  rld_destroy(e);
  return 0;
}
```
and to count a string in an FMD file:
```c
// compile with "gcc -O3 rld0.c this.c"; run with "./a.out idx.fmd AGCATAG"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rld0.h"
int main(int argc, char *argv[]) {
  if (argc < 3) return 1;
  rld_t *e = rld_restore(argv[1]);
  uint64_t k = 0, l = e->cnt[6], ok[6], ol[6];
  const char *s = argv[2];
  int i, len = strlen(s);
  for (i = len - 1; i >= 0; --i) { // backward search
    int c = s[i];
    c = c=='A'?1:c=='C'?2:c=='G'?3:c=='T'?4:5;
    rld_rank2a(e, k, l, ok, ol);
    k = e->cnt[c] + ok[c];
    l = e->cnt[c] + ol[c];
    if (k == l) break;
  }
  printf("%ld\n", (long)(l - k));
  rld_destroy(e);
  return 0;
}
```
-->

## <a name="limit"></a>Limitations

* Ropebwt3 is slow on the "locate" operation.

[grlbwt]: https://github.com/ddiazdom/grlBWT
[movi]: https://github.com/mohsenzakeri/Movi
[bigbwt]: https://gitlab.com/manzai/Big-BWT
[fm2]: https://github.com/lh3/fermi2
[rb2]: https://github.com/lh3/ropebwt2
[zenodo]: https://zenodo.org/records/11533210
[rb2-paper]: https://academic.oup.com/bioinformatics/article/30/22/3274/2391324
[fm-paper]: https://academic.oup.com/bioinformatics/article/28/14/1838/218887
[atb02]: https://ftp.ebi.ac.uk/pub/databases/AllTheBacteria/Releases/0.2/
[bwasw]: https://pubmed.ncbi.nlm.nih.gov/20080505/
