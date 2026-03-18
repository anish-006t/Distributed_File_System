#include "sentence.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

int split_sentences(const char *text, const char **out_sent_beg, size_t *out_sent_len, int max_sentences) {
    // If max_sentences == 0, or output arrays are NULL, perform a counting-only pass.
    int count = 0;
    size_t i = 0; size_t start = 0; size_t len = strlen(text);
    int store = (out_sent_beg != NULL && out_sent_len != NULL && max_sentences > 0);
    while (i < len && (!store || count < max_sentences)) {
        char c = text[i];
        if (c == '.' || c == '!' || c == '?') {
            size_t end = i + 1;
            while (end < len && (text[end] == '\n' || isspace((unsigned char)text[end]))) end++;
            if (store) {
                out_sent_beg[count] = text + start;
                out_sent_len[count] = (i + 1) - start;
            }
            count++;
            start = end;
            i = end;
        } else {
            i++;
        }
    }
    if (start < len && (!store || count < max_sentences)) {
        if (store) {
            out_sent_beg[count] = text + start;
            out_sent_len[count] = len - start;
        }
        count++;
    }
    return count;
}

int split_words(char *sentence, char **words, int max_words) {
    int count = 0;
    char *p = sentence;
    while (*p && count < max_words) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        words[count++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) { *p = '\0'; p++; }
    }
    return count;
}

char *insert_words_into_sentence(const char *sentence, int index, const char *to_insert) {
    // Build words list for existing sentence
    char *tmp = strdup(sentence);
    size_t slen = strlen(sentence);
    
    // Check if sentence ends with delimiter and preserve it
    char delimiter = '\0';
    if (slen > 0) {
        char last = tmp[slen-1];
        if (last == '.' || last == '!' || last == '?') {
            delimiter = last;
            tmp[--slen] = '\0';  // Remove delimiter temporarily
        }
    }
    
    // Remove trailing newline or spaces
    while (slen>0 && isspace((unsigned char)tmp[slen-1])) { tmp[--slen]='\0'; }
    
    char *words[4096];
    int wc = split_words(tmp, words, 4096);
    
    // Treat index 0 as index 1 (beginning of sentence) for user convenience
    if (index == 0) index = 1;
    // Convert from 1-based to 0-based for internal array indexing
    index = index - 1;
    
    if (index < 0) index = 0;
    if (index > wc) index = wc;

    // Count insert words
    char *ins = strdup(to_insert);
    char *iw[4096]; int iwc = split_words(ins, iw, 4096);

    // Compute size
    size_t newlen = 0;
    for (int i=0;i<wc;i++) newlen += strlen(words[i]) + 1;
    for (int i=0;i<iwc;i++) newlen += strlen(iw[i]) + 1;
    if (delimiter) newlen += 2;  // Space for delimiter
    if (newlen == 0) newlen = 1;

    char *out = (char*)malloc(newlen + 4);
    size_t pos = 0;
    // copy words before index
    for (int i=0;i<index;i++) {
        size_t l = strlen(words[i]); memcpy(out+pos, words[i], l); pos+=l; out[pos++]=' ';
    }
    // copy insert words
    for (int i=0;i<iwc;i++) { size_t l=strlen(iw[i]); memcpy(out+pos, iw[i], l); pos+=l; out[pos++]=' '; }
    // copy remaining words
    for (int i=index;i<wc;i++) { size_t l=strlen(words[i]); memcpy(out+pos, words[i], l); pos+=l; out[pos++]=' ';
    }
    if (pos>0 && out[pos-1]==' ') pos--; // trim last space
    
    // Re-add delimiter at the end if it existed
    if (delimiter) {
        out[pos++] = delimiter;
    }
    
    out[pos]='\0';
    free(tmp); free(ins);
    return out;
}
