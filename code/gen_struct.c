/* TODO(winston): error output */
/* TODO(winston): efficient memory usage */
/* TODO(winston): one-pass lexing */

#define GENERATED_EXTENSION ".h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "layer.h"

#ifdef _WIN32
#include "win32/time_util.h"
#elif __linux__
#include "linux/time_util.h"
#endif

enum Token_Type
{
	TOKEN_UNKNOWN,
	TOKEN_TEMPLATE,
	TOKEN_TEMPLATE_START,
	TOKEN_TEMPLATE_END,
	TOKEN_TEMPLATE_TYPE_NAME,
	TOKEN_TEMPLATE_NAME,
	TOKEN_TEMPLATE_NAME_STATEMENT,
	TOKEN_TEMPLATE_TYPE_INDICATOR,
	TOKEN_TEMPLATE_TYPE,
	TOKEN_IDENTIFIER,
	TOKEN_WHITESPACE,
	TOKEN_PARENTHETICAL_OPEN,
	TOKEN_PARENTHETICAL_CLOSE,
	TOKEN_BRACKET_OPEN,
	TOKEN_BRACKET_CLOSE,
	TOKEN_SEMICOLON,
	TOKEN_FEED_SYMBOL,
	TOKEN_END_OF_FILE
};

struct Token
{
	enum Token_Type token_type;
	char *token_data;
};

struct Tokenizer
{
	u32 token_num;
	struct Token *tokens;
	struct Token *at;
};

struct Template
{
	char template_name[16];
	char template_type_name[8];
	
	struct Tokenizer tokenizer;

	/* in case of name collisions */
	struct Template *next;
};

struct Template_Hash_Table
{
	struct Template *templates;
	u32 num;
};

struct Memory_Arena
{
	void *memory;
	
	u32 offset;
	u32 size;
	u32 size_left;
};

struct Template_Type_Request
{
	char *template_name;

	char **type_names;
	u32 num_of_types;
};

static struct Memory_Arena arena = {0};

/* djb2 hash function for string hashing */
static u64
get_hash(char *str)
{
	u64 hash = 5381;
	s32 c = 0;

	while ((c = *str++))
		hash = ((hash << 5) + hash) + c;

	return hash;
}

/* initializes global arena for program use */
static void
init_arena(void *memory, u32 size)
{
	arena.memory = memory;
	arena.size = size;
	arena.size_left = size;
}

/*
 * allocates specified amount of memory and
 * returns a pointer to the start of the memory
 */
static void *
alloc(u32 size)
{
	++size;
	
	void *result = 0;
	
	assert(arena.size_left >= size);
	
	result = &(((char *)arena.memory)[arena.offset]);
	
	arena.offset += size;
	arena.size_left -= size;
	memset(result, 0, size);
	
	return result;
}

/* resets arena offset but does not zero memory */
static inline void
clear_arena()
{
	arena.size_left = arena.size;
	arena.offset = 0;
}

/* utility function for getting the range between two indices */
static inline u32
get_range(u32 start, u32 end)
{
	return (end - start);
}

/* copies a specific range of input_string */
static void
copy_string_range(char *input_string, char *output_string, 
				  u32 start, u32 end)
{
	u32 string_length = get_range(start, end);
	
	for (u32 i = 0; i < string_length; ++i) {
		output_string[i] = input_string[i + start];
	}
	
	output_string[string_length] = '\0';
}

/*
 * debug printing for tokenizer
 * prints token type to specified FILE pointer.
 * utility function for print_token_string
 */
static void 
print_token_type(struct Token token, FILE *file)
{
	switch (token.token_type) {
#define TOKEN_PRINT_CASE(token_const)             \
case token_const:                                 \
	fprintf(file, "%s: ", #token_const);		  \
	break;
		TOKEN_PRINT_CASE(TOKEN_TEMPLATE);
		TOKEN_PRINT_CASE(TOKEN_TEMPLATE_START);
		TOKEN_PRINT_CASE(TOKEN_TEMPLATE_END);
		TOKEN_PRINT_CASE(TOKEN_TEMPLATE_TYPE_NAME);
		TOKEN_PRINT_CASE(TOKEN_TEMPLATE_TYPE);
		TOKEN_PRINT_CASE(TOKEN_TEMPLATE_NAME);
		TOKEN_PRINT_CASE(TOKEN_TEMPLATE_TYPE_INDICATOR);
		TOKEN_PRINT_CASE(TOKEN_IDENTIFIER);
		TOKEN_PRINT_CASE(TOKEN_WHITESPACE);
		TOKEN_PRINT_CASE(TOKEN_BRACKET_OPEN);
		TOKEN_PRINT_CASE(TOKEN_BRACKET_CLOSE);
		TOKEN_PRINT_CASE(TOKEN_PARENTHETICAL_OPEN);
		TOKEN_PRINT_CASE(TOKEN_PARENTHETICAL_CLOSE);
		TOKEN_PRINT_CASE(TOKEN_SEMICOLON);
		TOKEN_PRINT_CASE(TOKEN_END_OF_FILE);
		TOKEN_PRINT_CASE(TOKEN_FEED_SYMBOL);
#undef TOKEN_PRINT_CASE
	default:
		fprintf(file, "Unknown token: ");
		break;
	}
}

/* prints the token string out to specified stream */
static void
print_token_string(struct Token token, FILE *file)
{
	if (token.token_type == TOKEN_END_OF_FILE) {
		return;
	}
	
	if (token.token_type == TOKEN_WHITESPACE) {
		u32 string_length = strlen(token.token_data);
		for (u32 i = 0; i < string_length; ++i) {
			if (token.token_data[i] == '\n') {
				fprintf(file, "\\n");
			} else if (token.token_data[i] == '\t') {
				fprintf(file, "\\t");
			} else if (token.token_data[i] == ' ') {
				fprintf(file, "<space>");
			} else {
				fprintf(file, "%c", token.token_data[i]);
			}
		}
		return;
	}
	else {
		fprintf(file, "%s", token.token_data);
	}
}

/* prints the at pointer of the tokenizer */
static void
print_tokenizer_at(struct Tokenizer *tokenizer, FILE *file)
{
	print_token_type(*(tokenizer->at), file);
	print_token_string(*(tokenizer->at), file);
	fprintf(file, "\n");
}

/* resets the at pointer of the tokenizer to the starting point */
static inline void
reset_tokenizer(struct Tokenizer *tokenizer)
{
	tokenizer->at = tokenizer->tokens;
}

/*
 * increments the tokenizer at pointer, skips all whitespace and semicolons
 * returns FALSE if hits end of file or gets out of array bounds
 * returns TRUE if successfully incremented
 */
static bool
increment_tokenizer_no_whitespace(struct Tokenizer *tokenizer)
{
	do {
		if (tokenizer->at->token_type == TOKEN_END_OF_FILE ||
		    (tokenizer->at - tokenizer->tokens) >= tokenizer->token_num) {
			return FALSE;
		}
		++tokenizer->at;
	} while (tokenizer->at->token_type == TOKEN_WHITESPACE ||
			tokenizer->at->token_type == TOKEN_SEMICOLON);
	
	return TRUE;
}

/*
 * increments tokenizer by token, skipping nothing
 * returns FALSE if hits end of file or out of array bounds
 * returns TRUE if successfully incremented
 */
static bool
increment_tokenizer_all(struct Tokenizer *tokenizer)
{
	if (tokenizer->at->token_type == TOKEN_END_OF_FILE ||
	   (tokenizer->at - tokenizer->tokens) >= tokenizer->token_num) {
		return FALSE;
	}

	++tokenizer->at;
	return TRUE;
}

/*
 * utility function for getting the at pointer of the tokenizer
 * mainly for looks, not really needed though
 */
static inline struct Token *
get_tokenizer_at(struct Tokenizer *tokenizer)
{
	return tokenizer->at;
}

/*
 * gets the number of templates in a file
 * takes in a tokenized file
 * returns a u32 with the number of templates in file
 */
static u32
get_number_of_templates(struct Tokenizer *tokenizer)
{
	u32 count = 0;
	
	reset_tokenizer(tokenizer);
	
	do {
		if (get_tokenizer_at(tokenizer)->token_type == TOKEN_TEMPLATE_START) {
			++count;
		}
	} while (increment_tokenizer_no_whitespace(tokenizer));
	
	return count;
}

/*
 * gets the template name
 * takes in a tokenizer where the tokens pointer points to the first
 * token in the template
 */
static char *
get_template_name(struct Tokenizer *tokenizer)
{
	reset_tokenizer(tokenizer);
	
	do {
		if (get_tokenizer_at(tokenizer)->token_type == TOKEN_TEMPLATE_NAME) {
			reset_tokenizer(tokenizer);
			return get_tokenizer_at(tokenizer)->token_data;
		}
	} while (increment_tokenizer_no_whitespace(tokenizer));

	reset_tokenizer(tokenizer);
	return 0;
}

/*
 * takes in a template tokenizer and
 * returns the name of template
 */
static char *
get_template_type_name(struct Tokenizer *tokenizer)
{
	reset_tokenizer(tokenizer);
	
	do {
		if (get_tokenizer_at(tokenizer)->token_type ==
			TOKEN_TEMPLATE_TYPE_NAME) {
				reset_tokenizer(tokenizer);
				return get_tokenizer_at(tokenizer)->token_data;
		}
	} while(increment_tokenizer_no_whitespace(tokenizer));

	reset_tokenizer(tokenizer);
	return 0;
}

/*
 * gets struct Template from tokenizer where tokenizer starts at the
 * start of the template
 * TODO (winston): complete this function and maybe implement a more
 * versatile usage where the parameter does not have to point to the first
 * token in the template.
 */
static struct Template
get_template_from_tokens(struct Tokenizer *tokenizer)
{
	struct Template template = {0};
	
	strcpy(template.template_name,
		   get_template_name(tokenizer));
	strcpy(template.template_type_name,
		   get_template_type_name(tokenizer));
	
	struct Tokenizer template_tokenizer = {0};
	template_tokenizer.tokens = tokenizer->at;

	u32 range_start = tokenizer->at - tokenizer->tokens;
	u32 range_end = 0;

	reset_tokenizer(tokenizer);
	do {

	} while (increment_tokenizer_no_whitespace(tokenizer));
	
	template_tokenizer.token_num = get_range(range_start, range_end);
	template.tokenizer = template_tokenizer;

	reset_tokenizer(tokenizer);
	return template;
}

/*
 * Constructs a hash table from a file tokenizer
 * TODO (winston): complete this function
 */
static struct Template_Hash_Table
get_template_hash_table(struct Tokenizer *tokenizer)
{
	struct Template_Hash_Table hash_table = {0};
	hash_table.num = get_number_of_templates(tokenizer);
	hash_table.templates =
		(struct Template *)alloc(sizeof(struct Template) * hash_table.num);

	for (u32 i = 0; i < hash_table.num; ++i) {

	}

	return hash_table;
}

/*
 * gets number of template "type requests" in file
 * TODO (winston): maybe integrate this into the lexing process
 * to speed up things
 */
static u32
get_number_of_template_type_requests(struct Tokenizer tokenizer)
{
	u32 increment_thing = 0;

	reset_tokenizer(&tokenizer);
	do {
		if (get_tokenizer_at(&tokenizer)->token_type == TOKEN_TEMPLATE) {
			++increment_thing;
		}
	} while (increment_tokenizer_no_whitespace(&tokenizer));
	reset_tokenizer(&tokenizer);

	return increment_thing;
}

/*
 * gets the type of template requested
 */
static struct Template_Type_Request
get_template_type_request(struct Tokenizer file_tokens,
	                      char *template_name)
{
	struct Template_Type_Request type_request = {0};

	u32 num_of_template_requests =
		get_number_of_template_type_requests(file_tokens);

	type_request.type_names =
		(char **)alloc(sizeof(char *) * num_of_template_requests);

	reset_tokenizer(&file_tokens);
	do {
		if (get_tokenizer_at(&file_tokens)->token_type ==
		    TOKEN_TEMPLATE) {
			increment_tokenizer_no_whitespace(&file_tokens);

			if (strcmp(get_tokenizer_at(&file_tokens)->token_data,
				       template_name) != 0) {
				continue;
			}

			type_request.template_name =
				get_tokenizer_at(&file_tokens)->token_data;

			increment_tokenizer_no_whitespace(&file_tokens);
			increment_tokenizer_no_whitespace(&file_tokens);

			type_request.type_names[type_request.num_of_types] =
				get_tokenizer_at(&file_tokens)->token_data;
			++type_request.num_of_types;
		}
	} while (increment_tokenizer_no_whitespace(&file_tokens));
	reset_tokenizer(&file_tokens);

	return type_request;
}

/*
 * replace the type name in a template
 */
static void 
replace_type_name(struct Template *templates, char *type_name)
{
	reset_tokenizer(&templates->tokenizer);
	char *type_name_real = (char *)alloc(strlen(type_name));
	strcpy(type_name_real, type_name);
	
	do {
		if (((templates->tokenizer).at)->token_type == TOKEN_TEMPLATE_TYPE_NAME) {
			((templates->tokenizer).at)->token_data = type_name_real;
		}
	} while (increment_tokenizer_no_whitespace(&templates->tokenizer));
	reset_tokenizer(&templates->tokenizer);
}

/*
 * writes all contents of a template to specified file
 */
static void
write_template_to_file(struct Template *templates, FILE *file)
{
	reset_tokenizer(&templates->tokenizer);
	do {
		fprintf(file, "%s", ((templates->tokenizer).at)->token_data);
	} while (increment_tokenizer_all(&templates->tokenizer));
	reset_tokenizer(&templates->tokenizer);
}

/*
 * gets the pointer of the string to the next whitespace
 * or to the next semicolon or parenthesis
 */
static char *
get_string_to_next_whitespace(struct Token *tokens, 
							  char *file_data,
							  u32 *start_index)
{
	u32 range_start = *start_index;
	u32 range_end = *start_index;
	
	do {
		if (tokens[range_end].token_type == TOKEN_END_OF_FILE)
		{
			break;
		}
		++range_end;
	} while (tokens[range_end].token_type != TOKEN_WHITESPACE &&
			 tokens[range_end].token_type != TOKEN_SEMICOLON &&
			 tokens[range_end].token_type != TOKEN_PARENTHETICAL_OPEN &&
			 tokens[range_end].token_type != TOKEN_PARENTHETICAL_CLOSE);
	
	char *token_string = 
		(char *)alloc(get_range(range_start, range_end));
	
	for (u32 j = 0; j < get_range(range_start, range_end); ++j) {
		token_string[j] = file_data[range_start + j];
	}
	
	token_string[get_range(range_start, range_end)] = '\0';
	
	*start_index = range_end - 1;
	
	return token_string;
}

/*
 * does the opposite of the previous function
 * allocates string and points it to the from the start
 * and null-terminates at the next whitespace
 */
static char *
get_string_to_next_non_whitespace(struct Token *tokens, 
								  char *file_data,
								  u32 *start_index)
{
	u32 range_start = *start_index;
	u32 range_end = *start_index;
	
	do {
		if (tokens[range_end].token_type == TOKEN_END_OF_FILE) {
			break;
		}
		++range_end;
	} while (tokens[range_end].token_type == TOKEN_WHITESPACE &&
			 tokens[range_end].token_type == TOKEN_SEMICOLON &&
			 tokens[range_end].token_type != TOKEN_PARENTHETICAL_OPEN &&
			 tokens[range_end].token_type != TOKEN_PARENTHETICAL_CLOSE);
	
	char *token_string = 
		(char *)alloc(get_range(range_start, range_end));
	
	for (u32 j = 0; j < get_range(range_start, range_end); ++j) {
		token_string[j] = file_data[range_start + j];
	}
	
	token_string[get_range(range_start, range_end)] = '\0';
	
	*start_index = range_end - 1;
	
	return token_string;
}

/*
 * lexes the file and tokenizes all data
 * TODO (winston): split this into multiple functions
 * to shorten main function
 */
static struct Tokenizer
tokenize_file_data(char *file_data)
{
	u32 file_data_length = strlen(file_data) + 1;
	
	struct Tokenizer result = {0};
	
	struct Token *tokens = 
		(struct Token *)alloc(sizeof(*tokens) * file_data_length);
	
	for (u32 i = 0; i < file_data_length; ++i) {
		struct Token token;
		token.token_type = TOKEN_UNKNOWN;

		switch (file_data[i]) {
		case '\n':
		case ' ':
		case '\t':
			token.token_type = TOKEN_WHITESPACE;
			break;
		case '@':
			token.token_type = TOKEN_TEMPLATE;
			break;
		case '\0':
			token.token_type = TOKEN_END_OF_FILE;
			break;
		case '{':
			token.token_type = TOKEN_BRACKET_OPEN;
			break;
		case '}':
			token.token_type = TOKEN_BRACKET_CLOSE;
			break;
		case '(':
			token.token_type = TOKEN_PARENTHETICAL_OPEN;
			break;
		case ')':
			token.token_type = TOKEN_PARENTHETICAL_CLOSE;
			break;
		case ';':
			token.token_type = TOKEN_SEMICOLON;
			break;
		default:
			token.token_type = TOKEN_IDENTIFIER;
			break;
		}
		
		tokens[i] = token;
	}
	
	u32 counter = 0;
	for (u32 i = 0; i < file_data_length; ++i) {
		char *token_string = 0;

		switch (tokens[i].token_type) {
		case TOKEN_TEMPLATE:
		case TOKEN_SEMICOLON:
		case TOKEN_IDENTIFIER:
			tokens[counter].token_type = tokens[i].token_type;
			
			token_string = get_string_to_next_whitespace(tokens, 
										 file_data,
										 &i);
				break;
		case TOKEN_BRACKET_OPEN:
		case TOKEN_BRACKET_CLOSE:
		case TOKEN_PARENTHETICAL_OPEN:
		case TOKEN_PARENTHETICAL_CLOSE:
			tokens[counter].token_type = tokens[i].token_type;

			token_string = 
				(char *)alloc(2);

			token_string[0] = file_data[i];
			token_string[1] = '\0';			
			break;
		case TOKEN_WHITESPACE:
			tokens[counter].token_type = tokens[i].token_type;
			token_string = 
				get_string_to_next_non_whitespace(tokens, 
								  file_data,
								  &i);
			break;
		default:
			tokens[counter].token_type = tokens[i].token_type;
			break;
		}
		tokens[counter].token_data = token_string;

		++counter;
	}
	
	u32 tokens_actual_length = 0;
	for (u32 i = 0; i < file_data_length; ++i) {
		if (tokens[i].token_type == TOKEN_END_OF_FILE) {
			tokens_actual_length = i + 1;
			break;
		} else if (strcmp(tokens[i].token_data, "@template_start") == 0) {
			tokens[i].token_type = TOKEN_TEMPLATE_START;
		} else if (strcmp(tokens[i].token_data, "@template_end") == 0) {
			tokens[i].token_type = TOKEN_TEMPLATE_END;
		} else if (strcmp(tokens[i].token_data, "<-") == 0) {
			tokens[i].token_type = TOKEN_FEED_SYMBOL;
		} else if (strcmp(tokens[i].token_data, "->") == 0) {
			tokens[i].token_type = TOKEN_TEMPLATE_TYPE_INDICATOR;
		}
	}
	
	char *template_typename = 0;
	for (u32 i = 0; i < tokens_actual_length - 1; ++i) {
		if (tokens[i].token_type == TOKEN_TEMPLATE_START) {
			while (tokens[++i].token_type == TOKEN_WHITESPACE);
			tokens[i].token_type = TOKEN_TEMPLATE_NAME;
		}
		if (tokens[i].token_type == TOKEN_TEMPLATE_TYPE_INDICATOR) {
			while (tokens[++i].token_type == TOKEN_WHITESPACE);
			tokens[i].token_type = TOKEN_TEMPLATE_TYPE;
		}
		if (tokens[i].token_type == TOKEN_FEED_SYMBOL) {
			while (tokens[++i].token_type == TOKEN_WHITESPACE);
			
			tokens[i].token_type = TOKEN_TEMPLATE_TYPE_NAME;
			template_typename = tokens[i].token_data;
			continue;
		}
		if (template_typename) {
			if (strcmp(tokens[i].token_data, template_typename) == 0) {
				tokens[i].token_type = TOKEN_TEMPLATE_TYPE_NAME;
			}
		}
	}
	
	result.token_num = tokens_actual_length;
	result.tokens = tokens;
	result.at = result.tokens;
	
	return result;
}

/*
 * gets the file name from the path without the extension
 */
static char *
get_filename_no_ext(char *file_path)
{
	u32 file_path_length = strlen(file_path);
	u32 filename_start = 0;
	u32 filename_end = 0;
	
	for (u32 i = 0; i < file_path_length; ++i) {
		if (file_path[i] == '\\' ||
		    file_path[i] == '/') {
			filename_start = i + 1;
		}
		else if (file_path[i] == '.') {
			filename_end = i;
			break;
		}
	}
	
	u32 filename_length = get_range(filename_start, filename_end);
	char *filename = (char *)alloc(filename_length + 1);
	
	copy_string_range(file_path, filename,
					  filename_start, filename_end);
	
	return filename;
}

/*
 * gets the extension of the file from the file path
 */
static char *
get_file_ext(char *file_path)
{
	u32 file_path_length = strlen(file_path);
	
	u32 file_ext_start = 0;
	u32 file_ext_end = file_path_length;
	
	for (u32 i = 0; i < file_path_length; ++i) {
		if (file_path[i] == '.') {
			file_ext_start = i + 1;
			break;
		}
	}
	
	u32 file_ext_length = get_range(file_ext_start, file_ext_end);
	char *file_ext = (char *)alloc(file_ext_length + 1);
	
	copy_string_range(file_path, file_ext,
			  file_ext_start, file_ext_end);
	
	return file_ext;
}

/*
 * gets the directory from the file path
 */
static char *
get_file_working_dir(char *file_path)
{
	u32 file_path_length = strlen(file_path);
	
	u32 working_dir_start = 0;
	u32 working_dir_end = 0;
	
	for (u32 i = 0; i < file_path_length; ++i) {
		if (file_path[i] == '\\' || file_path[i] == '/') {
			working_dir_end = i + 1;
		}
	}
	
	u32 working_dir_length = get_range(working_dir_start, working_dir_end);
	char *working_dir = (char *)alloc(working_dir_length + 1);
	
	copy_string_range(file_path, working_dir,
			  working_dir_start, working_dir_end);
	
	return working_dir;
}

s32 main(s32 arg_count, char **args)
{
	if (arg_count < 2) {
		fprintf(stderr, "Specify file name as first argument");
		return -1;
	}
	
	f64 time_start = get_time();

	void *memory = malloc(512000);
	if (!memory)
		return -1;
	
	init_arena(memory, 512000);
	
	char *file_path = (char *)alloc(32);
	
	for (u32 i = 1; i < (u32)arg_count; ++i) {
		strcpy(file_path, args[i]);

		printf("%s -> ", file_path);

		char *filename_no_ext = get_filename_no_ext(file_path);
		char *file_working_dir = get_file_working_dir(file_path);
		char *output_file_path = (char *)alloc(128);
		
		strcpy(output_file_path, file_working_dir);
		strcat(output_file_path, filename_no_ext);
		strcat(output_file_path, GENERATED_EXTENSION);
		
		FILE *file = fopen(file_path, "r");
		
		if (!file) {
			continue;
		}
		
		fseek(file, 0, SEEK_END);
		u32 file_length = ftell(file);
		fseek(file, 0, SEEK_SET);
		
		char *file_contents = (char *)alloc(file_length);
		
		fread(file_contents, 1, file_length, file);
		
		fclose(file);
		
		struct Tokenizer tokenizer =
			tokenize_file_data(file_contents);

		FILE *output_file = fopen(output_file_path, "w");

		fclose(output_file);

		clear_arena();
		
		printf("%s\n", output_file_path);
	}
	
	free(memory);
	
	f64 time_end = get_time();
	
	printf("\nCode generation succeeded in %f seconds.\n",
		   time_end - time_start);

	return 0;
}