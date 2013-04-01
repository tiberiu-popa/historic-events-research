#include <algorithm>
#include <bitset>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <gsl/gsl_randist.h>
#include "dictionary_reader.h"
#include "dictionary_types.h"
#include "gaussian_finder.h"
#include "gaussian_model.h"
#include "linear_model.h"
#include "series.h"
#include "util.h"

using namespace std;

#define BUFFER_SIZE 1008
#define MAX_ENTRIES (1 << 20)

double average_match_count(struct series_entry *series, size_t start, size_t end)
{
	double acc = 0;
	for (size_t i = start; i < end; i++)
		acc += series[i].match_count;
	return acc / (end - start);
}

void append_csv(const char *filename, const char *word,
	int *counts, size_t num_elems)
{
	FILE *f;
	size_t i;

	f = fopen(filename, "at");
	if (f == NULL) {
		fprintf(stderr, "Could not open %s for writing\n", filename);
		exit(EXIT_FAILURE);
	}

#if 0
	fprintf(f, "\"%s\"", word);
	for (i = 0; i < num_elems; i++)
		fprintf(f, ",%d", counts[i]);
#else
	for (i = 0; i < num_elems; i++) {
		if (i > 0)
			fprintf(f, ",");
		fprintf(f, "%d", counts[i]);
	}
#endif
	fprintf(f, "\n");

	fclose(f);
}

void double_change_series_to_csv(const char *word, double *series)
{
	int counts[MAX_YEARS];

	memset(counts, 0, sizeof(counts));
	for (int j = 2; j < MAX_YEARS - 2; j++) {
		double a = series[j - 1];
		double b = series[j];
		double c = series[j + 1];
		if (a >= 0 && b >= 1e-4 && c >= 0) {
			int count = 0;
			while (count <= 8) {
				double sup_ratio = 1.0 + 0.1 * (count + 1);
				double dsup_ratio = 1.0 + 0.2 * (count + 1);
				double inf_ratio = 1.0 - 0.1 * (count + 1);
				double dinf_ratio = 1 / dsup_ratio;
				if ((b > sup_ratio * a && c > sup_ratio * b) || b > dsup_ratio * a ||
						(b < inf_ratio * a && c < inf_ratio * b) || b < dinf_ratio * a) {
					++count;
				} else {
					break;
				}
			}
			counts[j] = count;
		}
	}

	append_csv("data/relevance/double_change.csv", word, counts, MAX_YEARS);
}

void linear_model_series_to_csv(const char *word,
	const gsl_multimin_fdfminimizer_type *T,
	gsl_multimin_function_fdf *fdf)
{
	int counts[MAX_YEARS];
	struct static_array ranges;
	const int score_threshold = 3;

	memset(counts, 0, sizeof(counts));
	normalize_generate_ranges(&ranges, T, fdf);
	for (size_t i = 0; i < ranges.size; i++) {
		const struct range_entry *entry = &ranges.array[i];
		double score = -log(fabs(entry->slope));
		score = 2 * (score_threshold - score);
		int count = (int) score;
#if 0
		printf("%zu-%zu : %d (score=%lf slope=%lf)\n", entry->left, entry->right,
			count > 0 ? count : 0, score, entry->slope);
#endif
		if (count > 0) {
			for (size_t j = entry->left; j <= entry->right; j++)
				counts[j - MIN_YEAR] = count;
		}
	}

	append_csv("data/relevance/linear_model.csv", word, counts, MAX_YEARS);
}

void fit_gaussians(double *series, size_t inf, size_t sup, int *counts)
{
	vector<gaussian_entry> gaussians;
	bitset<MAX_YEARS> used;

	select_gaussians(series, inf, sup, gaussians);

	for (vector<gaussian_entry>::iterator it = gaussians.begin(); it != gaussians.end(); ++it) {
		size_t left = it->left;
		size_t right = it->right;
		bool valid = true;
		for (size_t i = left; i <= right; i++)
			if (used[i]) {
				valid = false;
				break;
			}
		if (valid) {
			double max_probability = gsl_ran_gaussian_pdf(0, it->sigma);
			double increase = it->increase;
			if (increase > 1.)
				increase = 1.;
			int count = (int) (10 * increase);
			//printf("count=%d\n", count);
			double ratio = (count + .5) / max_probability;
			for (size_t i = left; i <= right; i++) {
				used[i] = true;
				counts[i] = (int) (ratio * gsl_ran_gaussian_pdf(i - it->mean, 2 * it->sigma));
			}
		}
	}
}

void gaussian_model_series_to_csv(const char *word, double *series)
{
	int counts[MAX_YEARS];

	memset(counts, 0, sizeof(counts));
	fit_gaussians(series, 2, MAX_YEARS - 2, counts);

	append_csv("data/relevance/gaussian_model.csv", word, counts, MAX_YEARS);
}

int handle_entry(const struct dictionary_reader *dictreader, size_t index,
	const gsl_multimin_fdfminimizer_type *T,
	gsl_multimin_function_fdf regression_func,
	double *smooth_series)
{
	struct time_entry table[MAX_YEARS];
	double series[MAX_YEARS];
	const char *word;
	size_t table_size;
	int err;
	const unsigned int smoothing_window = 2;

	err = read_table(dictreader, index, table, &table_size);
	if (err != 0)
		goto out;

	table_to_series(dictreader, table, table_size, series);
	smoothify_series(series, smooth_series, MAX_YEARS, smoothing_window);

	word = dictreader->words[index];
	double_change_series_to_csv(word, smooth_series);
	gaussian_model_series_to_csv(word, smooth_series);
	linear_model_series_to_csv(word, T, &regression_func);

out:
	return err;
}

int string_compare(const void *a, const void *b)
{
	const char **x = (const char **) a;
	const char **y = (const char **) b;
	return strcmp(*x, *y);
}

int main()
{
	const gsl_multimin_fdfminimizer_type *T;
	gsl_multimin_function_fdf regression_func;

	double smooth_series[MAX_YEARS];
	struct dictionary_reader dict;
	const unsigned int smoothing_window = 2;
	struct static_range training_data = { 0, MAX_YEARS, 1500 + smoothing_window,
		/*{ 0.0, 0.0, 0.0, },*/
		smooth_series + smoothing_window, MAX_YEARS - 2 * smoothing_window };
	int err = 0;
	const char *words[] = {
		"war",
		"Microsoft",
#if 1
		"women",
		"communism",
		"atomic",
		"trench",
		"plague",
		"bomb",
		"Vietnam",
		"slavery",
		"famine",
		"Cuba",
		"terrorism",
		"assassination",
		"genocide",
		"revolution",
		"attack",
		"rebel",
		"fascism",
		"siege",
		"battle",
		"secession",
		"bombing",
		"Ceaucescu",
		"Jew",
#endif
	};

	T = gsl_multimin_fdfminimizer_conjugate_fr;

	regression_func.n = 2;
	regression_func.f = regression_f;
	regression_func.df = regression_df;
	regression_func.fdf = regression_fdf;
	regression_func.params = &training_data;

	err = init_dictreader(&dict, "data/sort/googlebooks-eng-all-1gram-20120701-database");
	if (err != 0)
		goto out;

	init_partial_sums();
	memset(smooth_series, 0, sizeof(smooth_series));

	remove("data/relevance/double_change.csv");
	remove("data/relevance/linear_model.csv");
	remove("data/relevance/gaussian_model.csv");
#if 0
	for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++) {
		void *p = bsearch(&words[i], dict.words, dict.num_words,
			sizeof(*dict.words), string_compare);
		if (p != NULL) {
			char **marker = (char **) p;
			ptrdiff_t index = marker - dict.words;
			err = handle_entry(&dict, (size_t) index, T, regression_func, smooth_series);
			if (err != 0)
				goto out_reader;
		}
	}
#else
	for (size_t i = 0; i < dict.num_words; i++) {
		err = handle_entry(&dict, i, T, regression_func, smooth_series);
		if (err != 0)
			goto out_reader;
	}
#endif
out_reader:
	destroy_dictreader(&dict);

out:
	return err;
}