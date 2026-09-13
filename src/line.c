/* Copyright (c) 2006-2026 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/tig.h"
#include "tig/types.h"
#include "tig/refdb.h"
#include "tig/line.h"
#include "tig/util.h"

static struct line_rule *line_rule;
static size_t line_rules;

static struct line_info **color_pair;
static size_t color_pairs;

DEFINE_ALLOCATOR(realloc_line_rule, struct line_rule, 8)
DEFINE_ALLOCATOR(realloc_color_pair, struct line_info *, 8)

enum line_type
get_line_type(const char *line)
{
	int linelen = strlen(line);
	enum line_type type;

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];

		if (rule->regex && !regexec(rule->regex, line, 0, NULL, 0))
			return type;

		/* Case insensitive search matches Signed-off-by lines better. */
		if (rule->linelen && linelen >= rule->linelen &&
		    !strncasecmp(rule->line, line, rule->linelen))
			return type;
	}

	return LINE_DEFAULT;
}

enum line_type
get_line_type_from_ref(const struct ref *ref)
{
	if (ref->type == REFERENCE_HEAD)
		return LINE_MAIN_HEAD;
	else if (ref->type == REFERENCE_LOCAL_TAG)
		return LINE_MAIN_LOCAL_TAG;
	else if (ref->type == REFERENCE_TAG)
		return LINE_MAIN_TAG;
	else if (ref->type == REFERENCE_TRACKED_REMOTE)
		return LINE_MAIN_TRACKED;
	else if (ref->type == REFERENCE_REMOTE)
		return LINE_MAIN_REMOTE;
	else if (ref->type == REFERENCE_STASH)
		return LINE_MAIN_STASH;
	else if (ref->type == REFERENCE_NOTE)
		return LINE_MAIN_NOTE;
	else if (ref->type == REFERENCE_PREFETCH)
		return LINE_MAIN_PREFETCH;
	else if (ref->type == REFERENCE_OTHER)
		return LINE_MAIN_OTHER;
	else if (ref->type == REFERENCE_REPLACE)
		return LINE_MAIN_REPLACE;

	return LINE_MAIN_REF;
}

const char *
get_line_type_name(enum line_type type)
{
	assert(0 <= type && type < line_rules);
	return line_rule[type].name;
}

struct line_info *
get_line_info(const char *prefix, enum line_type type)
{
	struct line_info *info;
	struct line_rule *rule;

	assert(0 <= type && type < line_rules);
	rule = &line_rule[type];
	for (info = &rule->info; info; info = info->next) {
		if (prefix && info->prefix == prefix)
			return info;
		if (!prefix && !info->prefix)
			return info;
	}

	return &rule->info;
}

static struct line_info *
init_line_info(const char *prefix, const char *name, size_t namelen, const char *line, size_t linelen, regex_t *regex)
{
	struct line_rule *rule;

	if (!realloc_line_rule(&line_rule, line_rules, 1))
		die("Failed to allocate line info");

	rule = &line_rule[line_rules++];
	rule->name = name;
	rule->namelen = namelen;
	rule->line = line;
	rule->linelen = linelen;
	rule->regex = regex;

	rule->info.prefix = prefix;
	rule->info.fg = COLOR_DEFAULT;
	rule->info.bg = COLOR_DEFAULT;

	return &rule->info;
}

#define INIT_BUILTIN_LINE_INFO(type, line) \
	init_line_info(NULL, #type, STRING_SIZE(#type), (line), STRING_SIZE(line), NULL)

static struct line_rule *
find_line_rule(struct line_rule *query)
{
	enum line_type type;

	if (!line_rules) {
		LINE_INFO(INIT_BUILTIN_LINE_INFO);
	}

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];

		if (query->namelen && enum_equals(*rule, query->name, query->namelen))
			return rule;

		if (query->linelen && query->linelen == rule->linelen &&
		    !strncasecmp(rule->line, query->line, rule->linelen))
			return rule;
	}

	return NULL;
}

struct line_info *
add_line_rule(const char *prefix, struct line_rule *query)
{
	struct line_rule *rule = find_line_rule(query);
	struct line_info *info, *last;

	if (!rule) {
		if (query->name)
			return NULL;

		return init_line_info(prefix, "", 0, query->line, query->linelen, query->regex);
	}

	/* When a rule already exists and we are just adding view-specific
	 * colors, query->line and query->regex can be freed. */
	free((void *) query->line);
	if (query->regex) {
		regfree(query->regex);
		free(query->regex);
	}

	for (info = &rule->info; info; last = info, info = info->next)
		if (info->prefix == prefix)
			return info;

	info = calloc(1, sizeof(*info));
	if (info)
		info->prefix = prefix;
	last->next = info;
	return info;
}

bool
foreach_line_rule(line_rule_visitor_fn visitor, void *data)
{
	enum line_type type;

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];

		if (!visitor(data, rule))
			return false;
	}

	return true;
}

static void
init_line_info_color_pair(struct line_info *info, enum line_type type,
	int default_bg, int default_fg)
{
	int bg = info->bg == COLOR_DEFAULT ? default_bg : info->bg;
	int fg = info->fg == COLOR_DEFAULT ? default_fg : info->fg;
	int i;

	for (i = 0; i < color_pairs; i++) {
		if (color_pair[i]->fg == info->fg && color_pair[i]->bg == info->bg) {
			info->color_pair = i;
			return;
		}
	}

	if (!realloc_color_pair(&color_pair, color_pairs, 1))
		die("Failed to allocate color pair");

	color_pair[color_pairs] = info;
	info->color_pair = color_pairs++;
	init_pair(COLOR_ID(info->color_pair), fg, bg);
}

/* Find the nearest color in the 6x6x6 color cube or the grayscale ramp. */
static int
rgb_to_256(int rgb)
{
	static const int levels[] = { 0, 95, 135, 175, 215, 255 };
	int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	int best = 16, best_dist = -1;
	int i;

	/* Colored input should not end up as gray, so skip the grayscale ramp. */
	int max = MAX(r, MAX(g, b)), min = MIN(r, MIN(g, b));
	int last = max - min > 16 ? 232 : 256;

	for (i = 16; i < last; i++) {
		int cr, cg, cb, dist;

		if (i < 232) {
			cr = levels[(i - 16) / 36];
			cg = levels[(i - 16) / 6 % 6];
			cb = levels[(i - 16) % 6];
		} else {
			cr = cg = cb = 8 + (i - 232) * 10;
		}

		dist = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb);
		if (best_dist < 0 || dist < best_dist) {
			best = i;
			best_dist = dist;
		}
	}

	return best;
}

static bool
color_is_used(int color)
{
	enum line_type type;

	for (type = 0; type < line_rules; type++) {
		struct line_info *info;

		for (info = &line_rule[type].info; info; info = info->next)
			if (info->fg == color || info->bg == color)
				return true;
	}

	return false;
}

/* Replace #rrggbb colors with redefined palette colors, or the nearest one. */
static void
init_rgb_color(int *color, int *next_slot)
{
	struct { int rgb, slot; } static seen[256];
	static int seen_count;
	int rgb = *color & ~COLOR_RGB_FLAG;
	int i;

	for (i = 0; i < seen_count; i++) {
		if (seen[i].rgb == rgb) {
			*color = seen[i].slot;
			return;
		}
	}

	if (!can_change_color() || COLORS < 256 || seen_count >= ARRAY_SIZE(seen)) {
		*color = rgb_to_256(rgb);
		return;
	}

	/* Take the least used palette colors, from the end of the color cube. */
	while (*next_slot >= 16 && color_is_used(*next_slot))
		(*next_slot)--;
	if (*next_slot < 16) {
		*color = rgb_to_256(rgb);
		return;
	}

	init_color(*next_slot, ((rgb >> 16) & 0xff) * 1000 / 255,
		   ((rgb >> 8) & 0xff) * 1000 / 255, (rgb & 0xff) * 1000 / 255);
	seen[seen_count].rgb = rgb;
	seen[seen_count++].slot = *next_slot;
	*color = (*next_slot)--;
}

void
init_colors(void)
{
	char *no_color = getenv("NO_COLOR");
	struct line_rule query = { "default", STRING_SIZE("default") };
	struct line_rule *rule = find_line_rule(&query);
	int default_bg = rule ? rule->info.bg : COLOR_BLACK;
	int default_fg = rule ? rule->info.fg : COLOR_WHITE;
	int next_slot = 231;
	enum line_type type;

	/* XXX: Even if the terminal does not support colors (e.g.
	 * TERM=dumb) init_colors() must ensure that the built-in rules
	 * have been initialized. This is done by the above call to
	 * find_line_rule(). */
	if (!has_colors() || (no_color != NULL && no_color[0] != '\0'))
		return;

	start_color();

	for (type = 0; type < line_rules; type++) {
		struct line_info *info;

		for (info = &line_rule[type].info; info; info = info->next) {
			if (COLOR_IS_RGB(info->fg))
				init_rgb_color(&info->fg, &next_slot);
			if (COLOR_IS_RGB(info->bg))
				init_rgb_color(&info->bg, &next_slot);
		}
	}

	if (rule) {
		default_bg = rule->info.bg;
		default_fg = rule->info.fg;
	}

	if (assume_default_colors(default_fg, default_bg) == ERR) {
		default_bg = COLOR_BLACK;
		default_fg = COLOR_WHITE;
	}

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];
		struct line_info *info;

		for (info = &rule->info; info; info = info->next) {
			init_line_info_color_pair(info, type, default_bg, default_fg);
		}
	}
}

/* vim: set ts=8 sw=8 noexpandtab: */
