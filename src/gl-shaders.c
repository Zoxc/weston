/*
 * Copyright © 2012 John Kåre Alsaker
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "gl-internal.h"

#define STRINGIFY(expr) #expr
#define STRINGIFY_VALUE(expr) STRINGIFY(expr)

static const size_t attribute_counts[ATTRIBUTE_COUNT] = {
	INPUT_COUNT,
	OUTPUT_COUNT,
	CONVERSION_COUNT
};

struct shader_builder;

typedef int (*gl_shader_constructor_t)
				  (struct shader_builder *builder);
typedef void (*gl_shader_setup_uniforms_t)(struct shader_builder *builder,
					   struct gl_shader *shader);

struct gl_input_type_desc {
	int transparent;
	gl_shader_constructor_t constructor;
	gl_shader_setup_uniforms_t setup_uniforms;
};

struct shader_string {
	size_t length;
	const char *str;
};

struct shader_string_list {
	struct shader_builder *builder;
	struct wl_array array;
};

struct shader_builder {
	struct gl_renderer *renderer;
	struct gl_input_type_desc *desc;
	int error;
	size_t attributes[ATTRIBUTE_COUNT];
	struct shader_string_list directives, global, body;
	size_t result_size;
	struct wl_array result;
};

static void
shader_string_list_init(struct shader_string_list *strings,
			struct shader_builder *builder)
{
	strings->builder = builder;
	wl_array_init(&strings->array);
}

static void
shader_builder_init(struct shader_builder *builder)
{
	builder->error = 0;
	builder->result_size = 1;
	wl_array_init(&builder->result);
	shader_string_list_init(&builder->directives, builder);
	shader_string_list_init(&builder->global, builder);
	shader_string_list_init(&builder->body, builder);
}

static void
shader_builder_release(struct shader_builder *builder)
{
	wl_array_release(&builder->directives.array);
	wl_array_release(&builder->global.array);
	wl_array_release(&builder->body.array);
}

static void
append_shader_string_list(char **result, struct shader_string_list *string)
{
	struct shader_string *str;

	wl_array_for_each(str, &string->array) {
		memcpy(*result, str->str, str->length);
		*result += str->length;
	}
}

static const char *
shader_builder_get_string(struct shader_builder *builder)
{
	char *data;

	if (builder->error)
		return NULL;

	data = wl_array_add(&builder->result, builder->result_size);

	if (!data)
		return NULL;

	append_shader_string_list(&data, &builder->directives);
	append_shader_string_list(&data, &builder->global);
	append_shader_string_list(&data, &builder->body);

	*data = 0;

	return builder->result.data;
}

static void
append(struct shader_string_list *list, const char *string)
{
	struct shader_string str;
	struct shader_string *data;

	if (!string) {
		list->builder->error = 1;
		return;
	}

	str.str = string;
	str.length = strlen(string);
	list->builder->result_size += str.length;

	if (str.length > list->builder->result_size)
		list->builder->error = 3;

	data = wl_array_add(&list->array, sizeof(str));
	if (!data)
		list->builder->error = 2;
	else
		*data = str;
}

static int
shader_rgbx_constructor(struct shader_builder *sb)
{
	append(&sb->global, "uniform sampler2D texture;\n");
	append(&sb->body,
		"gl_FragColor.rgb = texture2D(texture, texture_coord).rgb;\n" \
		"gl_FragColor.a = 1.0;\n");

	return 1;
}

static int
shader_rgba_constructor(struct shader_builder *sb)
{
	append(&sb->global, "uniform sampler2D texture;\n");
	append(&sb->body,
		"gl_FragColor = texture2D(texture, texture_coord);\n");


	return 1;
}

static int
shader_egl_external_constructor(struct shader_builder *sb)
{
	if (!sb->renderer->has_egl_image_external)
		return 0;

	append(&sb->directives,
		"#extension GL_OES_EGL_image_external : require;\n");
	append(&sb->global,
		"uniform samplerExternalOES texture;\n");
	append(&sb->body,
		"gl_FragColor = texture2D(texture, texture_coord);\n");

	return 1;
}

static void
shader_texture_uniforms(struct shader_builder *sb,
			struct gl_shader *shader)
{
	glUniform1i(glGetUniformLocation(shader->program, "texture"), 0);
}

static int
shader_yuv_constructor(struct shader_builder *sb)
{
	const char *sample;

	append(&sb->global,
		"uniform sampler2D planes[" STRINGIFY_VALUE(MAX_PLANES) "];\n");

	switch (sb->attributes[ATTRIBUTE_INPUT]) {
	case INPUT_Y_UV:
		sample = "vec3 yuv = vec3(" \
			"texture2D(planes[0], texture_coord).x," \
			"texture2D(planes[1], texture_coord).xy);\n";
		break;
	case INPUT_Y_U_V:
		sample = "vec3 yuv = vec3(" \
			"texture2D(planes[0], texture_coord).x," \
			"texture2D(planes[1], texture_coord).x," \
			"texture2D(planes[2], texture_coord).x);\n";
		break;
	case INPUT_Y_XUXV:
		sample = "vec3 yuv = vec3(" \
			"texture2D(planes[0], texture_coord).x," \
			"texture2D(planes[1], texture_coord).yw);\n";
		break;
	default:
		sample = NULL;
	}

	append(&sb->body, sample);
	append(&sb->body,
		"yuv = vec3(1.16438356 * (yuv.x - 0.0625), yuv.yz - 0.5);\n" \
		"gl_FragColor.r = yuv.x + 1.59602678 * yuv.z;\n" \
		"gl_FragColor.g = yuv.x - 0.39176229 * yuv.y - " \
			"0.81296764 * yuv.z;\n" \
		"gl_FragColor.b = yuv.x + 2.01723214 * yuv.y;\n" \
		"gl_FragColor.a = 1.0;\n");

	return 1;
}

static void
shader_yuv_uniforms(struct shader_builder *sb, struct gl_shader *shader)
{
	int i;
	GLint values[MAX_PLANES];

	for (i = 0; i < MAX_PLANES; i++)
		values[i] = i;

	glUniform1iv(glGetUniformLocation(shader->program, "planes"),
		     MAX_PLANES, values);
}

static int
shader_solid_constructor(struct shader_builder *sb)
{
	append(&sb->global, "uniform vec4 color;\n");
	append(&sb->body, "gl_FragColor = color;\n");

	return 1;
}

static void
shader_solid_uniforms(struct shader_builder *sb, struct gl_shader *shader)
{
	shader->color_uniform = glGetUniformLocation(shader->program, "color");
}

static struct gl_input_type_desc input_type_descs[INPUT_COUNT] = {
	/* INPUT_RGBX */
	{0, shader_rgbx_constructor, shader_texture_uniforms},

	/* INPUT_RGBA */
	{1, shader_rgba_constructor, shader_texture_uniforms},

	/* INPUT_EGL_EXTERNAL */
	{1, shader_egl_external_constructor, shader_texture_uniforms},

	/* INPUT_Y_UV */
	{0, shader_yuv_constructor, shader_yuv_uniforms},

	/* INPUT_Y_U_V */
	{0, shader_yuv_constructor, shader_yuv_uniforms},

	/* INPUT_Y_XUXV */
	{0, shader_yuv_constructor, shader_yuv_uniforms},

	/* INPUT_SOLID */
	{1, shader_solid_constructor, shader_solid_uniforms},
};

static void
attributes_from_permutation(size_t permutation, size_t *attributes)
{
	int i;
	for (i = 0; i < ATTRIBUTE_COUNT; i++) {
		size_t attribute_count = attribute_counts[i];
		size_t attribute = permutation % attribute_count;
		permutation /= attribute_count;
		attributes[i] = attribute;
	}
}

static size_t
permutation_from_attributes(size_t *attributes)
{
	size_t i;
	size_t result = 0, factor = 1;

	for (i = 0; i < ATTRIBUTE_COUNT; i++) {
		result += attributes[i] * factor;
		factor *= attribute_counts[i];
	}

	return result;
}

static const char vertex_shader_source[] =
	"uniform mat4 projection;\n"
	"attribute vec2 position;\n"
	"attribute vec2 attr_texture_coord;\n"
	"varying vec2 texture_coord;\n"
	"void main()\n"
	"{\n"
	"   gl_Position = projection * vec4(position, 0.0, 1.0);\n"
	"   texture_coord = attr_texture_coord;\n"
	"}\n";

static GLuint
compile_shader(GLenum type, const char *source)
{
	GLuint s;
	char msg[512];
	GLint status;

	s = glCreateShader(type);
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(s, sizeof msg, NULL, msg);
		weston_log("shader source: %s\n", source);
		weston_log("shader info: %s\n", msg);
		return GL_NONE;
	}

	return s;
}

static int
link_program(struct gl_renderer *gr,
	      struct gl_shader *shader,
	      const char *fragment_source)
{
	char msg[512];
	GLint status;
	GLuint program, fragment_shader;

	fragment_shader =
		compile_shader(GL_FRAGMENT_SHADER, fragment_source);

	if (!fragment_shader)
		return -1;

	program = glCreateProgram();

	glAttachShader(program, gr->vertex_shader);
	glAttachShader(program, fragment_shader);

	glDeleteShader(fragment_shader);

	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "attr_texture_coord");

	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &status);

	if (!status) {
		glGetProgramInfoLog(program, sizeof msg, NULL, msg);
		weston_log("link info: %s\n", msg);
		return -1;
	}

	shader->program = program;
	shader->projection_uniform = glGetUniformLocation(program,
							  "projection");
	shader->alpha_uniform = glGetUniformLocation(program, "alpha");

	return 0;
}

static int
create_shader(struct gl_renderer *gr,
	struct gl_shader *shader)
{
	struct shader_builder sb;
	const char *fragment_shader;

	attributes_from_permutation(shader->index, sb.attributes);

	sb.renderer = gr;
	sb.desc = &input_type_descs[sb.attributes[ATTRIBUTE_INPUT]];

	shader_builder_init(&sb);

	if (OPENGL_ES_VER)
		append(&sb.global, "precision mediump float;\n");
	append(&sb.global, "varying vec2 texture_coord;\n" \
		"uniform float alpha;\n");

	append(&sb.body, "void main()\n{\n");

	if (!sb.desc->constructor(&sb)) {
		shader_builder_release(&sb);
		return 0;
	}

	append(&sb.body, "gl_FragColor *= alpha;\n");

	if (gr->fragment_shader_debug)
		append(&sb.body, "gl_FragColor = vec4(0.0, 0.3, 0.0, 0.2) + " \
			"gl_FragColor * 0.8;\n");

	append(&sb.body, "}\n");

	fragment_shader = shader_builder_get_string(&sb);

	if (!fragment_shader)
		goto error;

	if (link_program(gr, shader, fragment_shader) < 0)
		goto error;

	glUseProgram(shader->program);

	sb.desc->setup_uniforms(&sb, shader);

	shader_builder_release(&sb);

	return 0;

error:
	shader_builder_release(&sb);
	return -1;
}

struct gl_shader *
gl_select_shader(struct gl_renderer *gr,
			enum gl_input_attribute input,
			enum gl_output_attribute output)
{
	struct gl_shader *shader;
	size_t index;
	size_t attributes[ATTRIBUTE_COUNT] = {
		input,
		output,
		CONVERSION_NONE
	};

	index = permutation_from_attributes(attributes);

	shader = gr->shaders[index];

	if (!shader) {
		shader = (gr->shaders[index] = calloc(1, sizeof(struct gl_shader)));
		shader->index = index;
	}

	assert(shader);

	return shader;
}

void
gl_use_shader(struct gl_renderer *gr,
			     struct gl_shader *shader)
{
	if (gr->current_shader == shader)
		return;

	if (shader->program == 0)
		create_shader(gr, shader);

	glUseProgram(shader->program);
	gr->current_shader = shader;
}

void
gl_shader_set_matrix(struct gl_shader *shader,
		     struct weston_matrix *matrix)
{
	GLfloat m[16];
	size_t i;

	for (i = 0; i < ARRAY_LENGTH(m); i++)
		m[i] = matrix->d[i];

	glUniformMatrix4fv(shader->projection_uniform,
			   1, GL_FALSE, m);
}

void
gl_shader_setup(struct gl_shader *shader,
		       struct weston_view *view,
		       struct weston_output *output)
{
	struct gl_surface_state *gs = get_surface_state(view->surface);

	gl_shader_set_matrix(shader, &output->matrix);

	if (gs->input == INPUT_SOLID)
		glUniform4fv(shader->color_uniform, 1, gs->color);

	glUniform1f(shader->alpha_uniform, view->alpha);
}

int
gl_init_shaders(struct gl_renderer *gr)
{
	struct gl_shader **shaders;
	size_t i, permutations = 1;

	for (i = 0; i < ATTRIBUTE_COUNT; i++)
		permutations *= attribute_counts[i];

	GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);

	if (!vertex_shader)
		return -1;

	shaders = calloc(permutations, sizeof(shaders));

	if (!shaders) {
		glDeleteShader(vertex_shader);
		return -1;
	}

	gl_destroy_shaders(gr);

	gr->vertex_shader = vertex_shader;
	gr->shaders = shaders;
	gr->shader_count = permutations;

	gr->solid_shader = gl_select_shader(gr, INPUT_SOLID, OUTPUT_BLEND);

	/* Force use_shader() to call glUseProgram(), since we need to use
	 * the recompiled version of the shader. */
	gr->current_shader = NULL;

	return 0;
}

void
gl_destroy_shaders(struct gl_renderer *gr)
{
	if (gr->shaders) {
		size_t i;

		for (i = 0; i < gr->shader_count; i++)
			if (gr->shaders[i]) {
				glDeleteProgram(gr->shaders[i]->program);
				free(gr->shaders[i]);
			}

		free(gr->shaders);

		glDeleteShader(gr->vertex_shader);
		gr->shaders = NULL;
	}
}
