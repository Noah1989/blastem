#import <AppKit/AppKit.h>
#include <stddef.h>
#include "../paths.h"
#include "../util.h"
#include "sfnt.h"

sfnt_table *find_font_in_dir(char *path, char *prefix, const char *ps_name)
{
	size_t num_entries;
	dir_entry *entries = get_dir_list(path, &num_entries);
	size_t prefix_len = strlen(prefix);
	sfnt_table *selected = NULL;
	for (size_t i = 0; i < num_entries && !selected; i++)
	{
		char *ext = path_extension(entries[i].name);
		if (!ext || (strcasecmp(ext, "ttf") && strcasecmp(ext, "ttc") && strcasecmp(ext, "dfont"))) {
			//not a truetype font, ignore
			free(ext);
			continue;
		}
		free(ext);
		if (!strncasecmp(entries[i].name, prefix, prefix_len)) {
			char *full_path = path_append(path, entries[i].name);
			FILE *f = fopen(full_path, "rb");
			if (f)
			{
				long font_size = file_size(f);
				uint8_t *blob = malloc(font_size);
				if (font_size == fread(blob, 1, font_size, f))
				{
					sfnt_container *sfnt = load_sfnt(blob, font_size);
					if (sfnt) {
						for (uint8_t j = 0; j < sfnt->num_fonts && !selected; j++)
						{
							char *cur_ps = sfnt_name(sfnt->tables + j, SFNT_POSTSCRIPT);
							if (!strcmp(cur_ps, ps_name)) {
								selected = sfnt->tables + j;
							}
							free(cur_ps);
						}
					} else {
						free(blob);
					}
				} else {
					free(blob);
				}
				fclose(f);
			}
			free(full_path);
		}
	}
	return selected;
}

uint8_t *default_font(uint32_t *size_out)
{
	NSFont *sys = [NSFont systemFontOfSize:0];
	NSString *name = [sys fontName];
	const char *ps_name = [name UTF8String];
	const unsigned char *prefix_start = (const unsigned char *)ps_name;
	while(*prefix_start && (
		*prefix_start < '0' || 
		(*prefix_start > 'z' && *prefix_start <= 0x80) || 
		(*prefix_start > 'Z' && *prefix_start < 'a') || 
		(*prefix_start > '9' && *prefix_start < 'A')
	))
	{
		prefix_start++;
	}
	if (!*prefix_start) {
		//Didn't find a suitable starting character, just start from the beginning
		prefix_start = (const unsigned char *)ps_name;
	}
	const unsigned char *prefix_end = (const unsigned char *)prefix_start + 1;
	while (*prefix_end && *prefix_end >= 'a')
	{
		prefix_end++;
	}
	char *prefix = malloc(prefix_end - prefix_start + 1);
	memcpy(prefix, prefix_start, prefix_end - prefix_start);
	prefix[prefix_end-prefix_start] = 0;
	//check /Library/Fonts first
	sfnt_table *selected = find_font_in_dir("/Library/Fonts", (char *)prefix, ps_name);
	if (!selected) {
		selected = find_font_in_dir("/System/Library/Fonts", (char *)prefix, ps_name);
	}
	if (!selected) {
		fatal_error("Failed to find system font %s using prefix %s\n", ps_name, prefix);
	}
	free(prefix);
	return sfnt_flatten(selected, size_out);
}

