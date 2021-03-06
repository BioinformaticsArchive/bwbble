//-------------------------------------------------------
// Common Alignment Functionality
// Victoria Popic (viq@stanford.edu), 2 Apr 2012
//-------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "bwt.h"
#include "align.h"
#include "exact_match.h"
#include "inexact_match.h"
#include "io.h"

void precalc_sa_intervals(bwt_t *BWT, const aln_params_t* params, char* preFname);
sa_intv_list_t* load_precalc_sa_intervals(const char* preFname);

void set_default_aln_params(aln_params_t* params) {
	params->gape_score = 4;
	params->gapo_score = 11;
	params->mm_score = 3;
	params->max_diff = 0;
	params->max_gape = 6;
	params->max_gapo = 1;
	params->seed_length = 32;
	params->max_diff_seed = 2;
	params->max_entries = 3000000;
	params->use_precalc = 0;
	params->matched_Ncontig = 0;
	params->is_multiref = 1;
	params->max_best = 30;
	params->no_indel_length = 5;
	params->n_threads = 1;
}

int align_reads(char* fastaFname, char* readsFname, aln_params_t* params) {
	printf("**** BWT-SNP Read Alignment ****\n");
	char* bwtFname  = (char*) malloc(strlen(fastaFname) + 5);
	char* alnsFname  = (char*) malloc(strlen(fastaFname) + 5);
	char* preFname = (char*) malloc(strlen(fastaFname) + 5);
	sprintf(bwtFname, "%s.bwt", fastaFname);
	sprintf(alnsFname, "%s.aln", fastaFname);
	sprintf(preFname, "%s.pre", fastaFname);
	remove(alnsFname); // remove an older .aln file (if it exists)

	clock_t t = clock();
	bwt_t* BWT = load_bwt_aln(bwtFname);
	printf("Total BWT loading time: %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);
	
	t = clock();
	reads_t* reads = fastq2reads(readsFname);
	printf("Total read loading time: %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);	

	sa_intv_list_t* sa_intv_table = NULL;
	if(params->use_precalc) {
		t = clock();
		if((FILE*) fopen(preFname, "r") == NULL) {
			precalc_sa_intervals(BWT, params, preFname);
		}
		sa_intv_table = load_precalc_sa_intervals(preFname);
		printf("Total pre-calculated intervals loading time: %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);
	}

	t = clock();
	//if(params->max_diff == 0) {
		//align_reads_exact(BWT, reads, sa_intv_table, params, alnsFname);
	//} else {
		if(params->n_threads > 1) {
			align_reads_inexact_parallel(BWT, reads, sa_intv_table, params, alnsFname);
		} else {
			align_reads_inexact(BWT, reads, sa_intv_table, params, alnsFname);
		}
	//}
	printf("Total read alignment time: %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);

	free_bwt(BWT);
	free_reads(reads);
	if(sa_intv_table) free_sa_interval_list(sa_intv_table);
	free(bwtFname);
	free(alnsFname);
	free(preFname);
	return 0;
}

/* SA Interval Management */

// intervals are always added in sorted order
// intervals cannot overlap but can be adjoining - adjoining intervals will be merged
void add_sa_interval(sa_intv_list_t* intv_list, bwtint_t L, bwtint_t U) {
	// adjoining intervals are merged
	if((intv_list->size != 0) && (L == (intv_list->last_intv->U + 1))) {
		intv_list->last_intv->U = U;
	} else {
		sa_intv_t* intv = (sa_intv_t*) calloc(1, sizeof(sa_intv_t));
		intv->L = L;
		intv->U = U;
		if(intv_list->size == 0) {
			intv_list->first_intv = intv;
			intv_list->last_intv = intv;
		} else {
			intv_list->last_intv->next_intv = intv;
			intv_list->last_intv = intv;
		}
		intv_list->size++;
	}
}

void clear_sa_interval_list(sa_intv_list_t* intv_list) {
	sa_intv_t* intv = intv_list->first_intv;
	for(int s = 0; s < intv_list->size; s++) {
		sa_intv_t* tmp = intv;
		intv = intv->next_intv;
		free(tmp);
	}
	intv_list->size = 0;
	intv_list->first_intv = 0;
	intv_list->last_intv = 0;
}

void free_sa_interval_list(sa_intv_list_t* intv_list) {
	sa_intv_t* intv = intv_list->first_intv;
	for(int s = 0; s < intv_list->size; s++) {
		sa_intv_t* tmp = intv;
		intv = intv->next_intv;
		free(tmp);
	}
	free(intv_list);
}

void print_sa_interval_list(sa_intv_list_t* intv_list) {
	if (intv_list->size != 0) {
		sa_intv_t* intv = intv_list->first_intv;
		for(int i = 0; i < intv_list->size; i++) {
			printf("SA Interval %d: L = %llu, U = %llu\n", i, intv->L, intv->U);
			intv = intv->next_intv;
		}
	}
}

void store_sa_interval_list(sa_intv_list_t* intv_list, FILE* saFile) {
	fwrite(&intv_list->size, sizeof(int), 1, saFile);
	sa_intv_t* intv = intv_list->first_intv;
	for(int s = 0; s < intv_list->size; s++) {
		fwrite(&intv->L, sizeof(bwtint_t), 1, saFile);
		fwrite(&intv->U, sizeof(bwtint_t), 1, saFile);
		intv = intv->next_intv;
	}
}

void load_sa_interval_list(sa_intv_list_t* intv_list, FILE* saFile) {
	fread(&intv_list->size, sizeof(int), 1, saFile);
	for(int j = 0; j < intv_list->size; j++) {
		sa_intv_t* intv = (sa_intv_t*) calloc(1, sizeof(sa_intv_t));
		fread(&intv->L, sizeof(bwtint_t), 1, saFile);
		fread(&intv->U, sizeof(bwtint_t), 1, saFile);
		if(j == 0) {
			intv_list->first_intv = intv;
			intv_list->last_intv = intv;
		} else {
			intv_list->last_intv->next_intv = intv;
			intv_list->last_intv = intv;
		}
	}
}
int read2index(char* read, int readLen) {
  int index = 0;
  for(int i = readLen-PRECALC_INTERVAL_LENGTH; i < readLen; i++) {
	  if(read[i] >= NUM_NUCLEOTIDES) {
		  // N's are treated as mismatches
		  return -1;
	  }
	  index *= NUM_NUCLEOTIDES;
	  index += read[i];
  }
  return index;
}
void next_read(read_t* read) {
	read->seq[read->len-1]++;
	for(int i = read->len - 1; i > 0; i--) {
		if(read->seq[i] < NUM_NUCLEOTIDES) {
			break;
		}
		read->seq[i] -= NUM_NUCLEOTIDES;
		read->seq[i-1]++;
	}
	if(read->seq[0] >= NUM_NUCLEOTIDES) {
		read->seq[0] -= NUM_NUCLEOTIDES;
	}
}

void precalc_sa_intervals(bwt_t *BWT, const aln_params_t* params, char* preFname) {
	printf("Pre-calculating SA intervals...\n");
	FILE* preFile = (FILE*)fopen(preFname, "wb");
	if (preFile == NULL) {
		fprintf(stderr, "precalc_sa_intervals: Cannot open PRE file %s!\n", preFname);
		exit(1);
	}
	sa_intv_list_t** sa_intervals = (sa_intv_list_t**) malloc(NUM_PRECALC*sizeof(sa_intv_list_t*));
	read_t *read = (read_t*) calloc(1, sizeof(read_t));
	read->len = PRECALC_INTERVAL_LENGTH;
	read->seq = (char*) calloc(read->len, sizeof(unsigned int));
	clock_t t = clock();
	for(int i = 0; i < NUM_PRECALC; i++) {
		exact_match(BWT, read, &sa_intervals[i], params);
		next_read(read);
	}
	printf("Interval pre-computation time: %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);
	t = clock();
	for(int i = 0; i < NUM_PRECALC; i++) {
		store_sa_interval_list(sa_intervals[i], preFile);
		free_sa_interval_list(sa_intervals[i]);
	}
	fclose(preFile);
	printf("Storing results time: %.2f sec\n", (float)(clock() - t) / CLOCKS_PER_SEC);
}

sa_intv_list_t* load_precalc_sa_intervals(const char* preFname) {
	FILE* preFile = (FILE*)fopen(preFname, "rb");
	if (preFile == NULL) {
		fprintf(stderr, "load_sa_intervals: Cannot open the PRE file: %s!\n", preFname);
		exit(1);
	}
	sa_intv_list_t* intv_list = (sa_intv_list_t*) malloc(NUM_PRECALC * sizeof(sa_intv_list_t));
	for(int i = 0; i < NUM_PRECALC; i++) {
		load_sa_interval_list(&intv_list[i], preFile);
	}
	fclose(preFile);
	return intv_list;
}

/* Alignments Management */

static inline int aln_score(const int m, const int o, const int e, const aln_params_t *p) {
	return m*p->mm_score + o*p->gapo_score + e*p->gape_score;
}

// allocate memory to store the alignments
alns_t* init_alignments() {
	alns_t* alns = (alns_t*) calloc(1, sizeof(alns_t));
	alns->num_entries = 0;
	alns->max_entries = 4;
	alns->entries = (aln_t*) calloc(alns->max_entries, sizeof(aln_t));
	if(alns == NULL || alns->entries == NULL) {
		printf("Could not allocate memory for alignments \n");
		exit(1);
	}
	return alns;
}

void free_alignments(alns_t* alns) {
	free(alns->entries);
	free(alns);
}

void reset_alignments(alns_t* alns) {
	alns->num_entries = 0;
}

void add_alignment(aln_entry_t* e, const bwtint_t L, const bwtint_t U, int score, alns_t* alns, const aln_params_t* params) {
	// do not add the alignment if we already have an alignment with these bounds (can occur with gaps)
	//if(params->max_gapo) {
	if(e->num_gapo) {
		for(int j = 0; j < alns->num_entries; j++) {
			aln_t aln = alns->entries[j];
			if(aln.L == L && aln.U == U) {
				return;
			}
		}
	}
	if(alns->num_entries == alns->max_entries) {
		alns->max_entries <<= 1;
		alns->entries = (aln_t*)realloc(alns->entries, alns->max_entries * sizeof(aln_t));
		memset(alns->entries + alns->max_entries/2, 0,  (alns->max_entries/2)*sizeof(aln_t));
	}
	aln_t *alt = &(alns->entries[alns->num_entries]);
	alt->num_mm = e->num_mm;
	alt->num_gapo = e->num_gapo;
	alt->num_gape = e->num_gape;
	alt->num_snps = e->num_snps;
	alt->L = L;
	alt->U = U;
	alt->score = score;
	alt->aln_length = e->aln_length;
	memcpy(&(alt->aln_path), &(e->aln_path), e->aln_length*sizeof(int));
	alns->num_entries++;
}

void print_alignments(alns_t* alns) {
	printf("Number of alignments = %d \n", alns->num_entries);
	for(int j = 0; j < alns->num_entries; j++) {
		aln_t aln = alns->entries[j];
		printf("Alignment %d: SA(%llu,%llu) score = %d, num_mm = %u, num_go = %u, num_ge = %u, num_snps = %u, aln_length = %d\n",
				j, aln.L, aln.U, aln.score, aln.num_mm, aln.num_gapo, aln.num_gape, aln.num_snps, 0);//, aln.aln_length);
		printf("\n");
	}
}

// create alignments for SA intervals from exact matching
alns_t* sa_intervals2alns(sa_intv_list_t* intv_list, int aln_length) {
	alns_t* alns = (alns_t*) calloc(1, sizeof(alns_t));
	alns->num_entries = intv_list->size;
	alns->entries = (aln_t*) calloc(alns->num_entries, sizeof(aln_t));
	if(alns == NULL || alns->entries == NULL) {
		printf("Could not allocate memory for alignments\n");
		exit(1);
	}
	sa_intv_t* intv = intv_list->first_intv;
	for(int i = 0; i < intv_list->size; i++) {
		aln_t* aln = &(alns->entries[i]);
		aln->L = intv->L;
		aln->U = intv->U;
		aln->aln_length = aln_length;
		// all other aln parameters are 0
		intv = intv->next_intv;
	}
	return alns;
}

// store alignments to file
void alns2alnf(alns_t* alns, FILE* alnFile) {
	fprintf(alnFile, "%d\n", alns->num_entries);
	for(int i = 0; i < alns->num_entries; i++) {
		aln_t aln = alns->entries[i];
		fprintf(alnFile, "%d\t%llu\t%llu\t%d\t%d\t%d\t%d\t", aln.score, aln.L, aln.U,
				aln.num_mm, aln.num_gapo, aln.num_gape, /*aln.num_snps,*/ aln.aln_length);
		for(int j = aln.aln_length-1; j >= 0; j--) {
			fprintf(alnFile, "%d ", aln.aln_path[j]);
		}
		fprintf(alnFile, "\n");
	}
}

// load alignments from file
alns_t* alnsf2alns(int* n_alns, char *alnFname) {
	FILE * alnFile = (FILE*) fopen(alnFname, "r");
	if (alnFile == NULL) {
		printf("alns2alnf: Cannot open ALN file: %s!\n", alnFname);
		perror(alnFname);
		exit(1);
	}
	int num_alns = 0;
	int alloc_alns = 2000000;
	alns_t* alns = (alns_t*) calloc(alloc_alns, sizeof(alns_t));

	while(!feof(alnFile)) {
		if(num_alns == alloc_alns) {
			alloc_alns <<= 1;
			alns = (alns_t*) realloc(alns, alloc_alns*sizeof(alns_t));
			memset(alns + alloc_alns/2, 0,  (alloc_alns/2)*sizeof(alns_t));
		}
		alns_t* read_alns = &alns[num_alns];
		fscanf(alnFile, "%d\n", &(read_alns->num_entries));
		read_alns->entries = (aln_t*) calloc(read_alns->num_entries, sizeof(aln_t));
		for(int i = 0; i < read_alns->num_entries; i++) {
			aln_t* aln = &(read_alns->entries[i]);
			fscanf(alnFile, "%d\t%llu\t%llu\t%d\t%d\t%d\t%d\t", &(aln->score), &(aln->L), &(aln->U),
					&(aln->num_mm), &(aln->num_gapo), &(aln->num_gape), /*&(aln->num_snps),*/ &(aln->aln_length));
			for(int j = 0; j < aln->aln_length; j++) {
				fscanf(alnFile, "%d ", &(aln->aln_path[j]));
			}
			fscanf(alnFile,"\n");
		}
		num_alns++;
	}
	*n_alns = num_alns;
	fclose(alnFile);
	return alns;
}

/* Alignment Result Evaluation */

int check_ref_mapping(read_t* read, int is_multiref);
void eval_aln(read_t* read, alns_t* alns, bwt_t* BWT, int is_multiref, int max_mm);

// Evaluate the alignment results of a given set of reads
void eval_alns(char *fastaFname, char *readsFname, char *alnsFname, int is_multiref, int max_diff) {
	printf("**** BWT-SNP Alignment Evaluation ****\n");

	// categorize reads (ids) by alignment type
	FILE* unalignedFile = (FILE*) fopen("bwbble.unaligned", "wb");
	FILE* confidentFile = (FILE*) fopen("bwbble.conf", "wb");
	FILE* correctFile = (FILE*) fopen("bwbble.corr", "wb");
	FILE* misalignedFile = (FILE*) fopen("bwbble.mis", "wb");
	if ((unalignedFile == NULL) || (confidentFile == NULL) || (correctFile == NULL) || (misalignedFile == NULL)) {
		printf("eval: Cannot open the unaligned/conf/corr/mis file(s)!\n");
		exit(1);
	}

	char* bwtFname  = (char*) malloc(strlen(fastaFname) + 5);
	sprintf(bwtFname, "%s.bwt", fastaFname);

	// load the alignment results of all the reads
	int num_alns;
	alns_t* alns = alnsf2alns(&num_alns, alnsFname);
	bwt_t* BWT = load_bwt(bwtFname);
	reads_t* reads = fastq2reads(readsFname);
	assert(num_alns == reads->count);

	// for each read: evaluate the alignment quality and accuracy
	int n_confident = 0;
	int n_correct = 0;
	int n_misaligned = 0;
	int n_unaligned = 0;

	for(int i = 0; i < reads->count; i++) {
		read_t* read = &(reads->reads[i]);
		parse_read_mapping(read);
		eval_aln(read, &alns[i], BWT, is_multiref, max_diff);
		if(read->aln_type == ALN_NOMATCH) {
			n_unaligned++;
			fwrite(&i, sizeof(int), 1, unalignedFile);
			continue;
		}
		if(read->mapQ < MAPQ_CONFIDENT) {
			continue;
		}
		n_confident++;
		fwrite(&i, sizeof(int), 1, confidentFile);
		if(check_ref_mapping(read, is_multiref) == 1) {
			n_correct++;
			fwrite(&i, sizeof(int), 1, correctFile);
		} else {
			n_misaligned++;
			fwrite(&i, sizeof(int), 1, misalignedFile);
		}
	}
	fwrite(&n_unaligned, sizeof(int), 1, unalignedFile);
	fwrite(&n_confident, sizeof(int), 1, confidentFile);
	fwrite(&n_correct, sizeof(int), 1, correctFile);
	fwrite(&n_misaligned, sizeof(int), 1, misalignedFile);

	printf("total num_reads = %d, confident = %d correct = %d, misaligned = %d, unaligned = %d\n", reads->count, n_confident, n_correct, n_misaligned, n_unaligned);

	free(bwtFname);
	free_reads(reads);
	free_alignments(alns);
	free_bwt(BWT);

	fclose(unalignedFile);
	fclose(confidentFile);
	fclose(correctFile);
	fclose(misalignedFile);
}

int mapq(const read_t *read, int max_mm, int is_multiref) {
	if (read->aln_top1_count == 0) return 23; // no hits
	if(is_multiref) {
		if (read->aln_top1_count > read->num_mref_pos) return 0; // repetitive top hit
	} else {
		if (read->aln_top1_count > (read->ref_pos_r - read->ref_pos_l + 1)) return 0;
	}
	if (read->num_mm == max_mm) return 25;
	if (read->aln_top2_count == 0) return 37; // unique
	int n = (read->aln_top2_count >= 255)? 255 : read->aln_top2_count;
	int q = (int)(4.343 * log(n) + 0.5);
	return (23 < q)? 0 : 23 - q;
}

int get_aln_length(int* aln_path, int path_length) {
	int aln_length = path_length;
	// discard all the insertions
	for(int i = 0; i < path_length; i++) {
		if(aln_path[i] == STATE_I) {
			aln_length--;
		}
	}
	return aln_length;
}

// Evaluate the alignment results of a given read
void eval_aln(read_t* read, alns_t* alns, bwt_t* BWT, int is_multiref, int max_mm) {
	// no matches
	if(alns->num_entries == 0) {
		read->aln_top1_count = 0;
		read->aln_top2_count = 0;
		read->aln_type = ALN_NOMATCH;
		return;
	}

	int best_score = alns->entries[0].score;
	for(int i = 0; i < alns->num_entries; i++) {
		aln_t aln = alns->entries[i];
		if(aln.score > best_score) {
			read->aln_top2_count += (aln.U - aln.L + 1);
		} else {
			// randomly select one of the top score alignments
			read->aln_top1_count += (aln.U - aln.L + 1);
			//if (drand48() * (aln.U - aln.L + 1 + read->aln_top1_count) > (double)read->aln_top1_count) {
			// pick only 1 top aln for this read
			if(i == 0) {
				read->num_mm = aln.num_mm;
				read->num_gapo = aln.num_gapo;
				read->num_gape = aln.num_gape;
				read->aln_score = aln.score;
				read->aln_length = aln.aln_length;
				memcpy(&(read->aln_path), aln.aln_path, aln.aln_length*sizeof(int));
				// randomly pick one of the matches
				read->aln_sa = aln.L + (bwtint_t)((aln.U - aln.L + 1) * drand48());
				// determine the position and strand of the mapping
				bwtint_t ref_pos = SA(BWT, read->aln_sa);
				if(ref_pos > (BWT->length-1)/2) {
					read->aln_strand = 0; // read rc + ref rc <=> read fwd + ref fwd
					bwtint_t fwd_pos = (BWT->length - 1) - ref_pos - 1;
					read->aln_pos = fwd_pos - get_aln_length(aln.aln_path, aln.aln_length) + 1;
				} else {
					read->aln_strand = 1; // read rc + fwd ref <=> fwd read/ref rc
					bwtint_t rc_pos = (BWT->length - 1) - ref_pos - 1;
					read->aln_pos = rc_pos - get_aln_length(aln.aln_path, aln.aln_length) + 1 - (BWT->length-1)/2;
				}
			}
		}
	}

	if(is_multiref) {
		read->aln_type = (read->aln_top1_count > read->num_mref_pos) ? ALN_REPEAT : ALN_UNIQUE;
	} else {
		read->aln_type = (read->aln_top1_count > read->ref_pos_r - read->ref_pos_l + 1) ? ALN_REPEAT : ALN_UNIQUE;
	}
	read->mapQ = mapq(read, max_mm, is_multiref);

	//printf("top1 = %d top2 = %d aln_score = %d\n", read->aln_top1_count, read->aln_top2_count, read->aln_score);
}

// 1 - correct, 0 - incorrect
int check_ref_mapping(read_t* read, int is_multiref) {
	// check strands
	if((read->aln_strand && !read->strand) || (!read->aln_strand && read->strand)) {
		return 0;
	}
	// check position
	if(is_multiref) {
		for(int i = 0; i < read->num_mref_pos; i++) {
			if(read->aln_pos == read->mref_pos[i] - 1) {
				return 1;
			}
		}
	} else {
		for(bwtint_t i = 0; i < read->ref_pos_r - read->ref_pos_l + 1; i++) {
			if(read->aln_pos == (read->ref_pos_l + i) - 1) {
				return 1;
			}
		}
	}
	return 0;
}
