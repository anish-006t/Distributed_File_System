#ifndef SENTENCE_H
#define SENTENCE_H

#include <stddef.h>

// Split content into sentences based on . ! ? terminators. Returns count and fills offsets
int split_sentences(const char *text, const char **out_sent_beg, size_t *out_sent_len, int max_sentences);

// Split sentence into words by spaces. Returns count, fills pointers.
int split_words(char *sentence, char **words, int max_words);

// Insert words at index into sentence; modifies original sentence buffer and returns new malloc'd string with result.
char *insert_words_into_sentence(const char *sentence, int index, const char *to_insert);

#endif // SENTENCE_H
