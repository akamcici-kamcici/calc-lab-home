// Ahmet Kamcici 241AEC014
// https://github.com/akamcik-kamcik/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: OpenAI ChatGPT, conversation assistance for reviewing and correcting
// tokenizer/parser behavior, error positions, file handling, and command-line validation.
// AI assistance was used in reviewing the code and proposing the fixes in this file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>
#include <limits.h>

/* The parser works on one line at a time, but token positions remain absolute
   1-based positions in the original file as required by the lab. */
typedef enum {
    TOK_EOF = 0, TOK_INT, TOK_FLOAT, TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV,
    TOK_POW, TOK_LPAREN, TOK_RPAREN, TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    union { long long i_val; double f_val; } value;
    size_t pos;
} Token;

typedef struct {
    int is_float;
    union { long long i_val; double f_val; } value;
    size_t pos;
} Value;

typedef struct {
    char *data;
    size_t length;
} FileBuffer;

static FileBuffer file_buffer = {NULL, 0};
static size_t current_pos = 0;       /* zero-based position in the whole file */
static size_t current_line_end = 0;  /* index of newline, or file length */
static Token current_token;
static int has_error = 0;
static size_t error_pos = 0;

/* Preserve the first error: later parser unwinding must not replace its position. */
static void set_error(size_t pos)
{
    if (!has_error) {
        has_error = 1;
        error_pos = pos;
    }
}

/* Read in growing chunks; doubling avoids repeatedly copying the entire file. */
static FileBuffer read_file(const char *filename)
{
    FileBuffer fb = {NULL, 0};
    FILE *f = fopen(filename, "rb");
    if (f == NULL) return fb;

    size_t capacity = 65536;
    char *data = malloc(capacity + 1);
    if (data == NULL) {
        fclose(f);
        return fb;
    }

    size_t length = 0;
    for (;;) {
        if (capacity - length < 4096) {
            if (capacity > SIZE_MAX / 2) {
                free(data);
                fclose(f);
                return fb;
            }
            size_t new_capacity = capacity * 2;
            char *grown = realloc(data, new_capacity + 1);
            if (grown == NULL) {
                free(data);
                fclose(f);
                return fb;
            }
            data = grown;
            capacity = new_capacity;
        }
        size_t got = fread(data + length, 1, capacity - length, f);
        length += got;
        if (got == 0) {
            if (ferror(f)) {
                free(data);
                fclose(f);
                return fb;
            }
            break;
        }
    }
    data[length] = '\0';
    fclose(f);
    fb.data = data;
    fb.length = length;
    return fb;
}

/* Token positions are recorded before consuming characters, so errors can
   identify the exact source character, including across earlier lines. */
static void advance_token(void)
{
    if (has_error) return;

    while (current_pos < current_line_end &&
           (file_buffer.data[current_pos] == ' ' ||
            file_buffer.data[current_pos] == '\t' ||
            file_buffer.data[current_pos] == '\r')) {
        current_pos++;
    }

    if (current_pos >= current_line_end) {
        current_token.type = TOK_EOF;
        current_token.pos = current_line_end + 1; /* newline position, 1-based */
        return;
    }

    size_t start = current_pos;
    char c = file_buffer.data[current_pos];
    current_token.pos = start + 1;

    if (isdigit((unsigned char)c)) {
        while (current_pos < current_line_end &&
               isdigit((unsigned char)file_buffer.data[current_pos])) {
            current_pos++;
        }

        if (current_pos < current_line_end && file_buffer.data[current_pos] == '.') {
            size_t dot_pos = current_pos;
            if (current_pos + 1 >= current_line_end ||
                !isdigit((unsigned char)file_buffer.data[current_pos + 1])) {
                current_token.type = TOK_ERROR;
                current_token.pos = dot_pos + 1;
                set_error(current_token.pos);
                return;
            }
            current_pos++;
            while (current_pos < current_line_end &&
                   isdigit((unsigned char)file_buffer.data[current_pos])) {
                current_pos++;
            }

            size_t n = current_pos - start;
            char *number = malloc(n + 1);
            if (number == NULL) {
                set_error(start + 1);
                current_token.type = TOK_ERROR;
                return;
            }
            memcpy(number, file_buffer.data + start, n);
            number[n] = '\0';
            errno = 0;
            char *endptr = NULL;
            double parsed = strtod(number, &endptr);
            if (errno == ERANGE || endptr == number || *endptr != '\0' || !isfinite(parsed)) {
                free(number);
                current_token.type = TOK_ERROR;
                set_error(start + 1);
                return;
            }
            free(number);
            current_token.type = TOK_FLOAT;
            current_token.value.f_val = parsed;
        } else {
            size_t n = current_pos - start;
            char *number = malloc(n + 1);
            if (number == NULL) {
                set_error(start + 1);
                current_token.type = TOK_ERROR;
                return;
            }
            memcpy(number, file_buffer.data + start, n);
            number[n] = '\0';
            errno = 0;
            char *endptr = NULL;
            long long parsed = strtoll(number, &endptr, 10);
            if (errno == ERANGE || endptr == number || *endptr != '\0') {
                free(number);
                current_token.type = TOK_ERROR;
                set_error(start + 1);
                return;
            }
            free(number);
            current_token.type = TOK_INT;
            current_token.value.i_val = parsed;
        }
        return;
    }

    current_pos++;
    switch (c) {
        case '+': current_token.type = TOK_PLUS; return;
        case '-': current_token.type = TOK_MINUS; return;
        case '/': current_token.type = TOK_DIV; return;
        case '(': current_token.type = TOK_LPAREN; return;
        case ')': current_token.type = TOK_RPAREN; return;
        case '*':
            if (current_pos < current_line_end && file_buffer.data[current_pos] == '*') {
                current_pos++;
                current_token.type = TOK_POW;
            } else {
                current_token.type = TOK_MUL;
            }
            return;
        default:
            current_token.type = TOK_ERROR;
            set_error(start + 1);
            return;
    }
}

static Value parse_expr(void);
static Value parse_term(void);
static Value parse_power(void);

static Value zero_value(size_t pos)
{
    Value v = {0};
    v.pos = pos;
    return v;
}

/* Parentheses preserve their opening position because that is the start of a
   grouped operand for division-by-zero and negative-exponent diagnostics. */
static Value parse_primary(void)
{
    Value v = zero_value(current_token.pos);
    if (has_error) return v;

    if (current_token.type == TOK_INT) {
        v.is_float = 0;
        v.value.i_val = current_token.value.i_val;
        advance_token();
    } else if (current_token.type == TOK_FLOAT) {
        v.is_float = 1;
        v.value.f_val = current_token.value.f_val;
        advance_token();
    } else if (current_token.type == TOK_LPAREN) {
        size_t opening_pos = current_token.pos;
        advance_token();
        v = parse_expr();
        if (has_error) return v;
        if (current_token.type != TOK_RPAREN) {
            set_error(current_token.pos);
            return v;
        }
        advance_token();
        v.pos = opening_pos;
    } else {
        set_error(current_token.pos);
    }
    return v;
}

static Value parse_power(void)
{
    Value base = parse_primary();
    if (has_error) return base;

    if (current_token.type == TOK_POW) {
        size_t op_pos = current_token.pos;
        advance_token();
        /* Recursing into parse_power makes exponentiation right-associative. */
        Value exp = parse_power();
        if (has_error) return base;

        Value result = zero_value(op_pos);
        if (base.is_float || exp.is_float) {
            double b = base.is_float ? base.value.f_val : (double)base.value.i_val;
            double e = exp.is_float ? exp.value.f_val : (double)exp.value.i_val;
            result.is_float = 1;
            errno = 0;
            result.value.f_val = pow(b, e);
            if (errno == ERANGE || !isfinite(result.value.f_val)) set_error(op_pos);
        } else {
            if (exp.value.i_val < 0) {
                set_error(exp.pos);
            } else {
                long long answer = 1;
                long long b = base.value.i_val;
                unsigned long long e = (unsigned long long)exp.value.i_val;
                /* Lab inputs do not overflow integer arithmetic. */
                while (e > 0) {
                    if (e & 1ULL) answer *= b;
                    e >>= 1;
                    if (e > 0) b *= b;
                }
                result.is_float = 0;
                result.value.i_val = answer;
            }
        }
        return result;
    }
    return base;
}

static Value parse_term(void)
{
    Value left = parse_power();
    if (has_error) return left;

    while (current_token.type == TOK_MUL || current_token.type == TOK_DIV) {
        TokenType op = current_token.type;
        size_t op_pos = current_token.pos;
        advance_token();
        Value right = parse_power();
        if (has_error) return left;

        Value result = zero_value(op_pos);
        if (left.is_float || right.is_float) {
            double l = left.is_float ? left.value.f_val : (double)left.value.i_val;
            double r = right.is_float ? right.value.f_val : (double)right.value.i_val;
            result.is_float = 1;
            if (op == TOK_DIV && r == 0.0) {
                set_error(right.pos);
            } else {
                result.value.f_val = (op == TOK_MUL) ? l * r : l / r;
                if (!isfinite(result.value.f_val)) set_error(op_pos);
            }
        } else {
            result.is_float = 0;
            if (op == TOK_DIV && right.value.i_val == 0) {
                set_error(right.pos);
            } else if (op == TOK_MUL) {
                result.value.i_val = left.value.i_val * right.value.i_val;
            } else {
                result.value.i_val = left.value.i_val / right.value.i_val;
            }
        }
        left = result;
        if (has_error) return left;
    }
    return left;
}

static Value parse_expr(void)
{
    Value left = parse_term();
    if (has_error) return left;

    while (current_token.type == TOK_PLUS || current_token.type == TOK_MINUS) {
        TokenType op = current_token.type;
        size_t op_pos = current_token.pos;
        advance_token();
        Value right = parse_term();
        if (has_error) return left;

        Value result = zero_value(op_pos);
        if (left.is_float || right.is_float) {
            double l = left.is_float ? left.value.f_val : (double)left.value.i_val;
            double r = right.is_float ? right.value.f_val : (double)right.value.i_val;
            result.is_float = 1;
            result.value.f_val = (op == TOK_PLUS) ? l + r : l - r;
            if (!isfinite(result.value.f_val)) set_error(op_pos);
        } else {
            result.is_float = 0;
            result.value.i_val = (op == TOK_PLUS)
                ? left.value.i_val + right.value.i_val
                : left.value.i_val - right.value.i_val;
        }
        left = result;
        if (has_error) return left;
    }
    return left;
}

/* Parse all expressions first and keep results in memory. If any line fails,
   the output file is replaced with only ERROR:<pos>, never partial results. */
static void process_file(const char *input_path, const char *output_path)
{
    FileBuffer fb = read_file(input_path);
    if (fb.data == NULL) {
        fprintf(stderr, "Error: Could not read input file %s\n", input_path);
        exit(EXIT_FAILURE);
    }

    size_t result_capacity = 16;
    size_t result_count = 0;
    char **results = malloc(result_capacity * sizeof(*results));
    if (results == NULL) {
        fprintf(stderr, "Error: Out of memory\n");
        free(fb.data);
        exit(EXIT_FAILURE);
    }

    file_buffer = fb;
    size_t line_start = 0;
    has_error = 0;
    error_pos = 0;

    while (line_start < fb.length) {
        size_t line_end = line_start;
        while (line_end < fb.length && fb.data[line_end] != '\n') line_end++;

        size_t first = line_start;
        while (first < line_end &&
               (fb.data[first] == ' ' || fb.data[first] == '\t' ||
                fb.data[first] == '\r' || fb.data[first] == '\f' || fb.data[first] == '\v')) {
            first++;
        }

        if (first != line_end && fb.data[first] != '#') {
            current_pos = line_start;
            current_line_end = line_end;
            has_error = 0;
            advance_token();
            Value result = parse_expr();
            if (!has_error && current_token.type != TOK_EOF) set_error(current_token.pos);

            if (has_error) {
                break;
            }

            char rendered[128];
            int n;
            if (result.is_float) n = snprintf(rendered, sizeof(rendered), "%.15g", result.value.f_val);
            else n = snprintf(rendered, sizeof(rendered), "%lld", result.value.i_val);
            if (n < 0 || (size_t)n >= sizeof(rendered)) {
                set_error(line_start + 1);
                break;
            }
            if (result_count == result_capacity) {
                size_t new_capacity = result_capacity * 2;
                char **grown = realloc(results, new_capacity * sizeof(*results));
                if (grown == NULL) {
                    fprintf(stderr, "Error: Out of memory\n");
                    for (size_t i = 0; i < result_count; i++) free(results[i]);
                    free(results);
                    free(fb.data);
                    exit(EXIT_FAILURE);
                }
                results = grown;
                result_capacity = new_capacity;
            }
            results[result_count] = malloc((size_t)n + 1);
            if (results[result_count] == NULL) {
                fprintf(stderr, "Error: Out of memory\n");
                for (size_t i = 0; i < result_count; i++) free(results[i]);
                free(results);
                free(fb.data);
                exit(EXIT_FAILURE);
            }
            memcpy(results[result_count], rendered, (size_t)n + 1);
            result_count++;
        }
        line_start = (line_end < fb.length) ? line_end + 1 : fb.length;
    }

    /* A trailing newline is the final line's boundary; no extra expression exists. */
    FILE *out = fopen(output_path, "w");
    if (out == NULL) {
        fprintf(stderr, "Error: Could not write output file %s\n", output_path);
        for (size_t i = 0; i < result_count; i++) free(results[i]);
        free(results);
        free(fb.data);
        exit(EXIT_FAILURE);
    }

    if (has_error) {
        fprintf(out, "ERROR:%zu\n", error_pos);
    } else {
        for (size_t i = 0; i < result_count; i++) fprintf(out, "%s\n", results[i]);
    }

    if (fclose(out) != 0) {
        fprintf(stderr, "Error: Could not finish writing output file %s\n", output_path);
        for (size_t i = 0; i < result_count; i++) free(results[i]);
        free(results);
        free(fb.data);
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < result_count; i++) free(results[i]);
    free(results);
    free(fb.data);
}

static void get_base_name(const char *path, char *out, size_t out_size)
{
    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    size_t len = strlen(filename);
    if (len >= 4 && strcmp(filename + len - 4, ".txt") == 0) len -= 4;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, filename, len);
    out[len] = '\0';
}

static int construct_output_name(const char *input_name, const char *first_name,
                                 const char *last_name, const char *student_id,
                                 char *out, size_t out_size)
{
    char base[512];
    get_base_name(input_name, base, sizeof(base));
    int n = snprintf(out, out_size, "%s_%s_%s_%s.txt", base, first_name, last_name, student_id);
    return n >= 0 && (size_t)n < out_size;
}

static int ensure_output_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 1;
        fprintf(stderr, "Error: Output path is not a directory: %s\n", path);
        return 0;
    }
    if (errno != ENOENT) {
        fprintf(stderr, "Error: Cannot access output directory %s\n", path);
        return 0;
    }
    if (mkdir(path, 0777) != 0) {
        fprintf(stderr, "Error: Could not create output directory %s\n", path);
        return 0;
    }
    return 1;
}

static int process_directory(const char *dir_path, const char *out_dir,
                             const char *first_name, const char *last_name,
                             const char *student_id)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        fprintf(stderr, "Error: Could not open directory %s\n", dir_path);
        return 0;
    }

    int ok = 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".txt") != 0) continue;

        char input_path[2048];
        int pn = snprintf(input_path, sizeof(input_path), "%s/%s", dir_path, entry->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(input_path)) {
            fprintf(stderr, "Error: Input path is too long\n");
            ok = 0;
            break;
        }
        struct stat st;
        if (stat(input_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        char output_name[1024];
        if (!construct_output_name(entry->d_name, first_name, last_name, student_id,
                                   output_name, sizeof(output_name))) {
            fprintf(stderr, "Error: Output filename is too long\n");
            ok = 0;
            break;
        }
        char output_path[2048];
        pn = snprintf(output_path, sizeof(output_path), "%s/%s", out_dir, output_name);
        if (pn < 0 || (size_t)pn >= sizeof(output_path)) {
            fprintf(stderr, "Error: Output path is too long\n");
            ok = 0;
            break;
        }
        process_file(input_path, output_path);
    }
    if (closedir(dir) != 0) ok = 0;
    return ok;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *out_dir = NULL;
    const char *input_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) {
            if (dir != NULL || input_file != NULL || i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Error: -d/--dir requires one directory and cannot be combined with an input file\n");
                return EXIT_FAILURE;
            }
            dir = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output-dir") == 0) {
            if (out_dir != NULL || i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Error: -o/--output-dir requires one directory\n");
                return EXIT_FAILURE;
            }
            out_dir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        } else {
            if (input_file != NULL || dir != NULL) {
                fprintf(stderr, "Error: Provide either one input file or -d DIR, not both\n");
                return EXIT_FAILURE;
            }
            input_file = argv[i];
        }
    }

    if ((dir == NULL && input_file == NULL) || (dir != NULL && input_file != NULL)) {
        fprintf(stderr, "Usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
        return EXIT_FAILURE;
    }

    const char *first_name = "ahmet";
    const char *last_name = "kamcici";
    const char *student_id = "241AEC014";
    char default_out_dir[1024];

    if (out_dir == NULL) {
        char base[512];
        get_base_name(dir != NULL ? dir : input_file, base, sizeof(base));
        int n = snprintf(default_out_dir, sizeof(default_out_dir), "%s_%s_%s",
                         base, first_name, student_id);
        if (n < 0 || (size_t)n >= sizeof(default_out_dir)) {
            fprintf(stderr, "Error: Default output directory name is too long\n");
            return EXIT_FAILURE;
        }
        out_dir = default_out_dir;
    }

    if (!ensure_output_directory(out_dir)) return EXIT_FAILURE;

    if (dir != NULL) {
        return process_directory(dir, out_dir, first_name, last_name, student_id)
            ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    char output_name[1024];
    if (!construct_output_name(input_file, first_name, last_name, student_id,
                               output_name, sizeof(output_name))) {
        fprintf(stderr, "Error: Output filename is too long\n");
        return EXIT_FAILURE;
    }
    char output_path[2048];
    int n = snprintf(output_path, sizeof(output_path), "%s/%s", out_dir, output_name);
    if (n < 0 || (size_t)n >= sizeof(output_path)) {
        fprintf(stderr, "Error: Output path is too long\n");
        return EXIT_FAILURE;
    }
    process_file(input_file, output_path);
    return EXIT_SUCCESS;
}
