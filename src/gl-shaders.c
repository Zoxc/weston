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

static void
add_conversion(struct shader_builder *sb)
{
	int alpha = sb->desc->transparent;

	if (sb->attributes[ATTRIBUTE_CONVERSION] != CONVERSION_FROM_SRGB)
		return;

	if (alpha)
		append(&sb->body,
			"gl_FragColor.rgb *= gl_FragColor.a > 0.0 ? " \
			"1.0 / gl_FragColor.a : 0.0;\n");

	append(&sb->global, "uniform sampler2D srgb_lut;\n");
	append(&sb->body,
		"gl_FragColor.rgb = gl_FragColor.rgb * 0.9473684210526316 + " \
			"0.02631578947368421;\n" \
		"gl_FragColor.rgb = vec3(" \
			"texture2D(srgb_lut, vec2(gl_FragColor.r, 0.5)).x," \
			"texture2D(srgb_lut, vec2(gl_FragColor.g, 0.5)).x," \
			"texture2D(srgb_lut, vec2(gl_FragColor.b, 0.5)).x);\n");

	if (alpha)
		append(&sb->body, "gl_FragColor.rgb *= gl_FragColor.a;\n");
}

static void
add_conversion_uniforms(struct shader_builder *builder,
			struct gl_shader *shader)
{
	if (builder->attributes[ATTRIBUTE_CONVERSION] != CONVERSION_FROM_SRGB)
		return;

	glUniform1i(glGetUniformLocation(shader->program, "srgb_lut"),
		MAX_PLANES);
}

static int
shader_rgbx_constructor(struct shader_builder *sb)
{
	append(&sb->global, "uniform sampler2D texture;\n");
	append(&sb->body,
		"gl_FragColor.rgb = texture2D(texture, texture_coord).rgb;\n");

	if (sb->attributes[ATTRIBUTE_OUTPUT] != OUTPUT_TO_SRGB)
		append(&sb->body, "gl_FragColor.a = 1.0;\n");

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
	append(&sb->body,  "yuv = yuv * vec3(1.16438356, 1.0, 0.81296764) - " \
			"vec3(0.07277397, 0.5, 0.40648382);\n" \
		"vec3 diff = vec3(yuv.x, yuv.x - yuv.z, 1.0);\n" \
		"gl_FragColor = yuv.zyyy * " \
			"vec4(1.96321071, -0.39176229, 2.01723214, 0.0) + " \
			"diff.xyxz;\n");

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
	if (sb->attributes[ATTRIBUTE_CONVERSION])
		return 0;

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
add_to_srgb_conversion(struct shader_builder *sb)
{
	append(&sb->global, "uniform sampler2D srgb_lut;\n");
	append(&sb->body,
		"gl_FragColor.rgb = gl_FragColor.rgb * 0.9946236559139785 + " \
			"0.002688172043010753;\n" \
		"gl_FragColor.rgb = vec3(" \
			"texture2D(srgb_lut, vec2(gl_FragColor.r, 0.5)).x," \
			"texture2D(srgb_lut, vec2(gl_FragColor.g, 0.5)).x," \
			"texture2D(srgb_lut, vec2(gl_FragColor.b, 0.5)).x);\n");

}

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

static struct gl_shader *
shader_create(GLuint vertex_shader,
		const char *fragment_source)
{
	char msg[512];
	GLint status;
	GLuint program, fragment_shader;

	struct gl_shader *shader;

	fragment_shader =
		compile_shader(GL_FRAGMENT_SHADER, fragment_source);

	if (!fragment_shader)
		return NULL;

	shader = calloc(1, sizeof(struct gl_shader));

	if (!shader)
		return NULL;

	program = glCreateProgram();

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);

	glDeleteShader(fragment_shader);

	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "attr_texture_coord");

	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &status);

	if (!status) {
		glGetProgramInfoLog(program, sizeof msg, NULL, msg);
		weston_log("link info: %s\n", msg);
		free(shader);
		return NULL;
	}

	shader->program = program;
	shader->projection_uniform = glGetUniformLocation(program,
							  "projection");
	shader->alpha_uniform = glGetUniformLocation(program, "alpha");

	return shader;
}


static void
destroy_shaders(struct gl_shader **shaders, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (shaders[i]) {
			glDeleteProgram(shaders[i]->program);
			free(shaders[i]);
		}

	free(shaders);
}

static int
create_shader_permutation(struct gl_renderer *renderer,
	struct gl_shader **shader, size_t permutation, GLuint vertex_shader)
{
	struct shader_builder sb;
	const char *fragment_shader;

	attributes_from_permutation(permutation, sb.attributes);

	sb.renderer = renderer;
	sb.desc = &input_type_descs[sb.attributes[ATTRIBUTE_INPUT]];

	if (sb.attributes[ATTRIBUTE_OUTPUT] == OUTPUT_TO_SRGB) {
		/* transparent inputs must be blended first */
		if (sb.desc->transparent)
			return 0;

		/* useless conversion from and to sRGB */
		if (sb.attributes[ATTRIBUTE_CONVERSION] == CONVERSION_FROM_SRGB)
			return 0;
	}

	shader_builder_init(&sb);

	if (OPENGL_ES_VER)
		append(&sb.global, "precision mediump float;\n");
	append(&sb.global, "varying vec2 texture_coord;\n");

	append(&sb.body, "void main()\n{\n");

	if (!sb.desc->constructor(&sb)) {
		shader_builder_release(&sb);
		return 0;
	}

	add_conversion(&sb);

	switch (sb.attributes[ATTRIBUTE_OUTPUT]) {
	case OUTPUT_TRANSPARENT:
		append(&sb.global, "uniform float alpha;\n");
		append(&sb.body, "gl_FragColor *= alpha;\n");
		break;
	case OUTPUT_TO_SRGB:
		add_to_srgb_conversion(&sb);
		break;
	default:
		break;
	}

	if (renderer->fragment_shader_debug &&
			sb.attributes[ATTRIBUTE_OUTPUT] != OUTPUT_TO_SRGB)
		append(&sb.body, "gl_FragColor = vec4(0.0, 0.3, 0.0, 0.2) + " \
			"gl_FragColor * 0.8;\n");

	append(&sb.body, "}\n");

	fragment_shader = shader_builder_get_string(&sb);

	if (!fragment_shader)
		goto error;

	*shader = shader_create(vertex_shader, fragment_shader);

	if (!*shader)
		goto error;

	glUseProgram((*shader)->program);

	sb.desc->setup_uniforms(&sb, *shader);

	add_conversion_uniforms(&sb, *shader);

	if (sb.attributes[ATTRIBUTE_OUTPUT] == OUTPUT_TO_SRGB)
		glUniform1i(glGetUniformLocation((*shader)->program, "srgb_lut"),
			MAX_PLANES);

	shader_builder_release(&sb);

	return 0;

error:
	shader_builder_release(&sb);
	return -1;
}

static struct gl_shader **
create_shader_permutations(struct gl_renderer *gr)
{
	struct gl_shader **shaders;
	size_t i, permutations = 1;
	unsigned int created = 0;
	GLuint vertex_shader;

	vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);

	if (!vertex_shader)
		return NULL;

	for (i = 0; i < ATTRIBUTE_COUNT; i++)
		permutations *= attribute_counts[i];

	shaders = calloc(permutations, sizeof(shaders));

	if (!shaders)
		return NULL;

	for (i = 0; i < permutations; i++) {
		if (create_shader_permutation(gr, &shaders[i],
				i, vertex_shader) < 0)
			goto error;

		if (shaders[i])
			created++;
	}

	gr->shader_count = permutations;

	weston_log("Created %u shader permutations\n", created);

	glDeleteShader(vertex_shader);

	return shaders;

error:
	destroy_shaders(shaders, permutations);
	glDeleteShader(vertex_shader);
	return NULL;
}

struct gl_shader *
gl_select_shader(struct gl_renderer *gr,
			enum gl_input_attribute input,
			enum gl_output_attribute output,
			enum gl_conversion_attribute conversion)
{
	struct gl_shader *shader;
	size_t attributes[ATTRIBUTE_COUNT] = {
		input,
		output,
		conversion
	};

	shader = gr->shaders[permutation_from_attributes(attributes)];

	assert(shader);

	return shader;
}

void
gl_use_shader(struct gl_renderer *gr,
			     struct gl_shader *shader)
{
	if (gr->current_shader == shader)
		return;

	glUseProgram(shader->program);
	gr->current_shader = shader;
}

void
gl_shader_set_output(struct gl_shader *shader,
		     struct weston_output *output)
{
	GLfloat matrix[16];
	size_t i;

	for (i = 0; i < ARRAY_LENGTH(matrix); i++)
		matrix[i] = output->matrix.d[i];

	glUniformMatrix4fv(shader->projection_uniform,
			   1, GL_FALSE, matrix);
}

void
gl_shader_setup(struct gl_shader *shader,
		       struct weston_surface *surface,
		       struct weston_output *output)
{
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_surface_state *gs = get_surface_state(surface);

	gl_shader_set_output(shader, output);

	if (gs->input == INPUT_SOLID)
		glUniform4fv(shader->color_uniform, 1, gs->color);

	if (gs->conversion == CONVERSION_FROM_SRGB) {
		glActiveTexture(GL_TEXTURE0 + MAX_PLANES);
		glBindTexture(GL_TEXTURE_2D, gr->srgb_decode_lut);
	}

	glUniform1f(shader->alpha_uniform, surface->alpha);
}

static void
setup_lut(GLuint *texture, const void *data,
	GLsizei entries, GLenum internal_format, GLenum type)
{
	glGenTextures(1, texture);

	glBindTexture(GL_TEXTURE_2D, *texture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexImage2D(GL_TEXTURE_2D, 0, internal_format, entries, 1, 0,
		GL_LUMINANCE, type, data);
}

static const uint16_t srgb_decode_lut[] = {
	0, 281, 751, 1519, 2618, 4073, 5919, 8166, 10847, 13984, 17589, 21690,
	26301, 31424, 37095, 43321, 50125, 57488, 65535
};

static const uint8_t srgb_encode_lut[] = {
	0, 17, 27, 34, 40, 46, 50, 55, 59, 62, 66, 69, 72, 75, 78, 80, 83, 85,
	88, 90, 92, 95, 97, 99, 101, 103, 105, 107, 108, 110, 112, 114, 115,
	117, 119, 120, 122, 124, 125, 127, 128, 130, 131, 132, 134, 135, 137,
	138, 139, 141, 142, 143, 145, 146, 147, 148, 149, 151, 152, 153, 154,
	156, 156, 158, 159, 160, 161, 162, 163, 165, 165, 166, 168, 168, 170,
	170, 172, 172, 174, 174, 176, 176, 178, 178, 180, 181, 181, 183, 183,
	185, 185, 186, 187, 188, 189, 190, 190, 192, 192, 194, 194, 195, 196,
	196, 198, 198, 200, 200, 201, 202, 203, 203, 204, 205, 206, 207, 207,
	208, 209, 210, 211, 211, 212, 213, 214, 214, 215, 216, 217, 217, 218,
	219, 221, 218, 222, 222, 223, 223, 224, 225, 226, 226, 227, 228, 228,
	229, 231, 229, 231, 232, 232, 233, 234, 235, 235, 236, 237, 237, 238,
	239, 239, 240, 241, 241, 242, 242, 243, 245, 242, 245, 247, 245, 247,
	248, 248, 249, 249, 250, 252, 250, 252, 253, 253, 255, 252, 255
};

static void
setup_luts(struct gl_renderer *gr)
{
	setup_lut(&gr->srgb_decode_lut, srgb_decode_lut,
		ARRAY_LENGTH(srgb_decode_lut),
		gr->l16_internal_format, GL_UNSIGNED_SHORT);

	setup_lut(&gr->srgb_encode_lut, srgb_encode_lut,
		ARRAY_LENGTH(srgb_encode_lut), GL_LUMINANCE, GL_UNSIGNED_BYTE);
}

int
gl_init_shaders(struct gl_renderer *gr)
{
	if (gl_compile_shaders(gr) < 0)
		return -1;

	setup_luts(gr);

	return 0;
}

int
gl_compile_shaders(struct gl_renderer *gr)
{
	struct gl_shader **shaders = create_shader_permutations(gr);

	if (!shaders)
		return -1;

	if (gr->shaders)
		gl_destroy_shaders(gr);

	gr->shaders = shaders;
	gr->solid_shader = gl_select_shader(gr,
		INPUT_SOLID,
		OUTPUT_BLEND,
		CONVERSION_NONE);

	return 0;
}

void
gl_destroy_shaders(struct gl_renderer *gr)
{
	destroy_shaders(gr->shaders, gr->shader_count);
}
