#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <argp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
CONTAINER_HEADER_SIZE = 36,
BLOB_HEADER_SIZE = 32,
BLOB_ID_VIDEO = 0x10,
BLOB_ID_VIDEO_BLANK = 0x11,
BLOB_ID_AUDIO = 0x20,
BLOB_ID_AUDIO_BLANK = 0x21,
BLOB_ID_JPEG_TABLES = 0xe0,
BLOB_ID_VBI = 0xd0
};

const char *argp_program_version = "ldimg-decode 1.1";
static char doc[] =
    "Extract video fields or lossless PCM audio from a Dexter LDIMG file.";
static char args_doc[] = "FILE";

static struct argp_option options[] = {
    {"video", 'v', 0, 0, "Extract JPEG fields and create an uncompressed AVI (default).", 0},
    {"jpeg-only", 'j', 0, 0, "Extract JPEG fields without creating the AVI.", 0},
    {"batch-size", 's', "N", 0, "Set number of fields per batch directory (default 1000).", 0},
    {"wav", 'w', 0, 0, "Extract audio as uncompressed 44.1 kHz PCM WAV.", 0},
    {"flac", 'f', 0, 0, "Extract audio as lossless FLAC.", 0},
    {"merge", 'm', 0, 0, "Mux the extracted AVI and WAV into one AVI.", 0},
    {"vbi", 'b', 0, 0, "Export the original VBI blob byte-for-byte.", 0},
    {"vbi-mame", 'M', 0, 0, "Export VBI converted to MAME's 16-byte per-field layout.", 0},
    {0}
};

struct arguments {
    const char *input_path;
    bool make_avi;
    uint64_t batch_size; /* number of fields per batch directory */
    bool extract_audio;
    bool audio_flac;
    bool merge_av;
    bool export_vbi;
    bool vbi_mame;
};

struct container {
    unsigned char md5[16];
    uint64_t blob_count;
    uint64_t toc_offset;
    uint64_t *blob_offsets;
};

struct jpeg_info {
    uint16_t width;
    uint16_t height;
    bool has_dqt;
    bool has_dht;
};

static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;

    switch (key) {
    case 'v':
        arguments->merge_av = false;
        arguments->export_vbi = false;
        arguments->extract_audio = false;
        arguments->make_avi = true;
        break;
    case 'j':
        arguments->merge_av = false;
        arguments->export_vbi = false;
        arguments->extract_audio = false;
        arguments->make_avi = false;
        break;
    case 'w':
        arguments->merge_av = false;
        arguments->export_vbi = false;
        arguments->extract_audio = true;
        arguments->audio_flac = false;
        break;
    case 'f':
        arguments->merge_av = false;
        arguments->export_vbi = false;
        arguments->extract_audio = true;
        arguments->audio_flac = true;
        break;
    case 'm':
        arguments->merge_av = true;
        arguments->export_vbi = false;
        arguments->extract_audio = false;
        arguments->make_avi = false;
        break;
    case 'b':
        arguments->merge_av = false;
        arguments->export_vbi = true;
        arguments->vbi_mame = false;
        arguments->extract_audio = false;
        arguments->make_avi = false;
        break;
    case 'M':
        arguments->merge_av = false;
        arguments->export_vbi = true;
        arguments->vbi_mame = true;
        arguments->extract_audio = false;
        arguments->make_avi = false;
        break;
    case 's': {
        if (!arg) return ARGP_ERR_UNKNOWN;
        char *endptr = NULL;
        unsigned long long v = strtoull(arg, &endptr, 10);
        if (endptr == arg || *endptr != '\0' || v == 0) {
            fprintf(stderr, "Invalid batch-size value: %s\n", arg);
            return ARGP_ERR_UNKNOWN;
        }
        arguments->batch_size = (uint64_t)v;
        break;
    }
    case ARGP_KEY_ARG:
        if (arguments->input_path != NULL)
            argp_usage(state);
        arguments->input_path = arg;
        break;
    case ARGP_KEY_END:
        if (arguments->input_path == NULL)
            argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

static uint32_t
read_u32_le(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
read_u64_le(const unsigned char *p)
{
    return (uint64_t)read_u32_le(p) | (uint64_t)read_u32_le(p + 4) << 32;
}

static bool
seek_to(FILE *file, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX) {
        fprintf(stderr, "File offset is too large: %" PRIu64 "\n", offset);
        return false;
    }
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
        fprintf(stderr, "Could not seek to offset %" PRIu64 ": %s\n",
                offset, strerror(errno));
        return false;
    }
    return true;
}

static bool
read_exact(FILE *file, void *buffer, size_t size, const char *description)
{
    if (fread(buffer, 1, size, file) != size) {
        fprintf(stderr, "Could not read %s: %s\n", description,
                ferror(file) ? strerror(errno) : "unexpected end of file");
        return false;
    }
    return true;
}

static bool
load_container(FILE *file, uint64_t file_size, struct container *container)
{
    unsigned char header[CONTAINER_HEADER_SIZE];
    if (!seek_to(file, 0) || !read_exact(file, header, sizeof(header), "container header"))
        return false;
    if (memcmp(header, "1CON", 4) != 0) {
        fprintf(stderr, "Input is not an MPO container (expected 1CON magic).\n");
        return false;
    }

    memcpy(container->md5, header + 4, sizeof(container->md5));
    container->blob_count = read_u64_le(header + 20);
    container->toc_offset = read_u64_le(header + 28);
    if (container->blob_count == 0 || container->toc_offset < CONTAINER_HEADER_SIZE ||
        container->toc_offset > file_size ||
        container->blob_count > (file_size - container->toc_offset) / 8 ||
        container->toc_offset + container->blob_count * 8 != file_size ||
        container->blob_count > (container->toc_offset - CONTAINER_HEADER_SIZE) /
                                    BLOB_HEADER_SIZE ||
        container->blob_count > SIZE_MAX / sizeof(*container->blob_offsets)) {
        fprintf(stderr, "Invalid container table-of-contents bounds.\n");
        return false;
    }

    size_t toc_size = (size_t)container->blob_count * 8;
    unsigned char *toc = malloc(toc_size);
    container->blob_offsets = malloc((size_t)container->blob_count *
                                     sizeof(*container->blob_offsets));
    if (toc == NULL || container->blob_offsets == NULL) {
        fprintf(stderr, "Not enough memory to read the blob index.\n");
        free(toc);
        return false;
    }
    if (!seek_to(file, container->toc_offset) ||
        !read_exact(file, toc, toc_size, "container table of contents")) {
        free(toc);
        return false;
    }
    for (uint64_t i = 0; i < container->blob_count; i++) {
        container->blob_offsets[i] = read_u64_le(toc + (size_t)i * 8);
        if ((i == 0 && container->blob_offsets[i] != CONTAINER_HEADER_SIZE) ||
            (i > 0 && container->blob_offsets[i] <= container->blob_offsets[i - 1])) {
            fprintf(stderr, "Invalid blob offset in table entry %" PRIu64 ".\n", i);
            free(toc);
            return false;
        }
    }
    free(toc);
    return true;
}

static void
print_metadata(const struct container *container, uint64_t file_size,
               uint64_t first_blob_size, uint32_t first_blob_id)
{
    printf("LDIMG metadata:\n");
    printf("  Container version: 1CON\n");
    printf("  MD5 hash: ");
    for (size_t i = 0; i < sizeof(container->md5); i++)
        printf("%02x", container->md5[i]);
    printf("\n");
    printf("  Number of blobs: %" PRIu64 "\n", container->blob_count);
    printf("  Blob table of contents offset: %" PRIu64 " (0x%" PRIx64 ")\n",
           container->toc_offset, container->toc_offset);
    printf("  First blob starts at offset: %" PRIu64 " (0x%" PRIx64 ")\n",
           container->blob_offsets[0], container->blob_offsets[0]);
    printf("  First blob ID: 0x%08" PRIx32 ", payload size: %" PRIu64 " bytes\n",
           first_blob_id, first_blob_size);
    printf("  File size: %" PRIu64 " bytes\n\n", file_size);
}

static bool
read_blob_header(FILE *file, const struct container *container, uint64_t index,
                 uint64_t *payload_size, uint32_t *blob_id)
{
    unsigned char header[BLOB_HEADER_SIZE];
    uint64_t offset = container->blob_offsets[index];
    uint64_t next = index + 1 < container->blob_count
                        ? container->blob_offsets[index + 1]
                        : container->toc_offset;
    if (offset > next || next - offset < BLOB_HEADER_SIZE ||
        !seek_to(file, offset) ||
        !read_exact(file, header, sizeof(header), "blob header"))
        return false;

    *payload_size = read_u64_le(header);
    *blob_id = read_u32_le(header + 28);
    if (*payload_size != next - offset - BLOB_HEADER_SIZE) {
        fprintf(stderr, "Blob %" PRIu64 " size does not match the next index offset.\n",
                index);
        return false;
    }
    return true;
}

static bool
read_blob_payload(FILE *file, const struct container *container, uint64_t index,
                  uint64_t payload_size, unsigned char **payload)
{
    if (payload_size > SIZE_MAX) {
        fprintf(stderr, "Blob %" PRIu64 " is too large for this process.\n", index);
        return false;
    }
    unsigned char *data = malloc((size_t)payload_size);
    if (data == NULL && payload_size != 0) {
        fprintf(stderr, "Not enough memory to read blob %" PRIu64 ".\n", index);
        return false;
    }
    if (!seek_to(file, container->blob_offsets[index] + BLOB_HEADER_SIZE) ||
        !read_exact(file, data, (size_t)payload_size, "blob payload")) {
        free(data);
        return false;
    }
    *payload = data;
    return true;
}

static char *output_stem(const char *input_path);
static char *append_suffix(const char *stem, const char *suffix);
static char *reserve_temp_output(const char *output_path);
static bool publish_temp_output(const char *temp_path, const char *output_path);

static bool
write_audio_bytes(FILE *output, const unsigned char *data, size_t size)
{
    if (size != 0 && fwrite(data, 1, size, output) != size) {
        fprintf(stderr, "Could not write audio output: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static bool
write_silence(FILE *output, uint64_t byte_count)
{
    static const unsigned char silence[8192] = {0};
    while (byte_count != 0) {
        size_t count = byte_count < sizeof(silence)
                           ? (size_t)byte_count : sizeof(silence);
        if (!write_audio_bytes(output, silence, count))
            return false;
        byte_count -= count;
    }
    return true;
}

static bool
write_wav_header(FILE *output, uint64_t pcm_size, uint16_t channels)
{
    unsigned char header[44] = {0};
    if (pcm_size > UINT32_MAX - 36) {
        fprintf(stderr, "PCM audio is too large for a standard WAV file; use --flac.\n");
        return false;
    }
    memcpy(header, "RIFF", 4);
    header[4] = (unsigned char)(pcm_size + 36);
    header[5] = (unsigned char)((pcm_size + 36) >> 8);
    header[6] = (unsigned char)((pcm_size + 36) >> 16);
    header[7] = (unsigned char)((pcm_size + 36) >> 24);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16;
    header[20] = 1;
    header[22] = (unsigned char)channels;
    header[24] = 0x44;
    header[25] = 0xac;
    uint32_t byte_rate = 44100U * channels * 2U;
    header[28] = (unsigned char)byte_rate;
    header[29] = (unsigned char)(byte_rate >> 8);
    header[30] = (unsigned char)(byte_rate >> 16);
    header[31] = (unsigned char)(byte_rate >> 24);
    uint16_t block_align = channels * 2;
    header[32] = (unsigned char)block_align;
    header[33] = (unsigned char)(block_align >> 8);
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    header[40] = (unsigned char)pcm_size;
    header[41] = (unsigned char)(pcm_size >> 8);
    header[42] = (unsigned char)(pcm_size >> 16);
    header[43] = (unsigned char)(pcm_size >> 24);
    return write_audio_bytes(output, header, sizeof(header));
}

static bool
write_audio_payload(FILE *output, FILE *input,
                    const struct container *container, uint64_t index,
                    uint64_t payload_size, uint32_t blob_id)
{
    if (blob_id == BLOB_ID_AUDIO_BLANK) {
        unsigned char blank_header[4];
        if (payload_size != sizeof(blank_header) ||
            !seek_to(input, container->blob_offsets[index] + BLOB_HEADER_SIZE) ||
            !read_exact(input, blank_header, sizeof(blank_header),
                        "blank-audio length"))
            return false;
        uint64_t silent_bytes = read_u32_le(blank_header);
        if (silent_bytes % 2 != 0) {
            fprintf(stderr, "Blank-audio blob %" PRIu64
                            " has an odd byte count.\n", index);
            return false;
        }
        return write_silence(output, silent_bytes);
    }

    if (payload_size > SIZE_MAX) {
        fprintf(stderr, "Audio blob %" PRIu64 " is too large for this process.\n",
                index);
        return false;
    }
    unsigned char *payload = NULL;
    if (!read_blob_payload(input, container, index, payload_size, &payload))
        return false;
    bool ok = payload_size % 2 == 0 &&
              write_audio_bytes(output, payload, (size_t)payload_size);
    if (payload_size % 2 != 0)
        fprintf(stderr, "Audio blob %" PRIu64 " has an odd PCM byte count.\n", index);
    free(payload);
    return ok;
}

static bool
start_ffmpeg_flac(const char *output_path, unsigned channels,
                  FILE **audio_stream, pid_t *child_pid)
{
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        fprintf(stderr, "Could not create audio pipe: %s\n", strerror(errno));
        return false;
    }

    char channel_arg[16];
    snprintf(channel_arg, sizeof(channel_arg), "%u", channels);
    char *const argv[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-f", "s16le", "-ar", "44100", "-ac", channel_arg, "-i", "pipe:0",
        "-c:a", "flac", "-compression_level", "8", "-f", "flac",
        (char *)output_path, NULL
    };
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "Could not start ffmpeg: %s\n", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }
    if (child == 0) {
        close(pipe_fds[1]);
        signal(SIGPIPE, SIG_DFL);
        if (dup2(pipe_fds[0], STDIN_FILENO) < 0) {
            fprintf(stderr, "Could not connect audio stream to ffmpeg: %s\n",
                    strerror(errno));
            _exit(127);
        }
        close(pipe_fds[0]);
        execvp(argv[0], argv);
        fprintf(stderr, "Could not execute ffmpeg: %s\n", strerror(errno));
        _exit(127);
    }
    close(pipe_fds[0]);
    FILE *output = fdopen(pipe_fds[1], "wb");
    if (output == NULL) {
        fprintf(stderr, "Could not open audio pipe stream: %s\n", strerror(errno));
        close(pipe_fds[1]);
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        return false;
    }
    *audio_stream = output;
    *child_pid = child;
    return true;
}

static bool
finish_ffmpeg_flac(FILE *audio_stream, pid_t child_pid)
{
    bool ok = true;
    if (fflush(audio_stream) != 0) {
        fprintf(stderr, "Could not flush audio to ffmpeg: %s\n", strerror(errno));
        ok = false;
    }
    if (fclose(audio_stream) != 0) {
        fprintf(stderr, "Could not close audio stream: %s\n", strerror(errno));
        ok = false;
    }

    int status;
    while (waitpid(child_pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        fprintf(stderr, "Could not wait for ffmpeg: %s\n", strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ffmpeg failed to encode FLAC audio.\n");
        ok = false;
    }
    return ok;
}

static bool
metadata_is_pal(FILE *input, const struct container *container)
{
    uint64_t size;
    uint32_t id;
    unsigned char *payload = NULL;
    if (!read_blob_header(input, container, 0, &size, &id) ||
        id != 0xf0 || size > 16384 ||
        !read_blob_payload(input, container, 0, size, &payload)) {
        free(payload);
        return false;
    }

    bool pal = false;
    if (size >= 16 && memcmp(payload, "1LDI", 4) == 0) {
        pal = read_u32_le(payload + 12) != 1;
    } else if (size > 4 && memcmp(payload, "2LDI", 4) == 0) {
        char *text = malloc((size_t)size - 3);
        if (text != NULL) {
            memcpy(text, payload + 4, (size_t)size - 4);
            text[size - 4] = '\0';
            char *type = strstr(text, "\"type\"");
            if (type != NULL) {
                char *value = strchr(type, ':');
                if (value != NULL) {
                    value++;
                    while (*value == ' ' || *value == '\t' || *value == '\r' ||
                           *value == '\n')
                        value++;
                    if (*value == '"')
                        pal = strncmp(value + 1, "PAL", 3) == 0;
                }
            }
            free(text);
        }
    }
    free(payload);
    return pal;
}

static int
compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static bool
extract_audio(const struct arguments *arguments)
{
    FILE *input = fopen(arguments->input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Could not open input file %s: %s\n",
                arguments->input_path, strerror(errno));
        return false;
    }
    if (fseeko(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "Could not determine input file size: %s\n", strerror(errno));
        fclose(input);
        return false;
    }
    off_t end = ftello(input);
    if (end < 0) {
        fprintf(stderr, "Could not determine input file size: %s\n", strerror(errno));
        fclose(input);
        return false;
    }

    struct container container = {0};
    if (!load_container(input, (uint64_t)end, &container)) {
        free(container.blob_offsets);
        fclose(input);
        return false;
    }

    uint64_t audio_bytes = 0;
    uint64_t audio_count = 0;
    uint64_t blank_count = 0;
    uint64_t observed_sizes[40];
    size_t observed_count = 0;
    uint64_t first_blob_size = 0;
    uint32_t first_blob_id = 0;
    bool scan_ok = true;

    for (uint64_t i = 0; i < container.blob_count; i++) {
        uint64_t payload_size;
        uint32_t blob_id;
        if (!read_blob_header(input, &container, i, &payload_size, &blob_id)) {
            scan_ok = false;
            break;
        }
        if (i == 0) {
            first_blob_size = payload_size;
            first_blob_id = blob_id;
        }
        if (blob_id == BLOB_ID_AUDIO) {
            if (payload_size % 2 != 0 || UINT64_MAX - audio_bytes < payload_size) {
                fprintf(stderr, "Invalid or oversized PCM audio blob at index %" PRIu64 ".\n", i);
                scan_ok = false;
                break;
            }
            audio_bytes += payload_size;
            audio_count++;
            if (observed_count < sizeof(observed_sizes) / sizeof(observed_sizes[0]))
                observed_sizes[observed_count++] = payload_size;
        } else if (blob_id == BLOB_ID_AUDIO_BLANK) {
            unsigned char blank_header[4];
            if (payload_size != sizeof(blank_header) ||
                !seek_to(input, container.blob_offsets[i] + BLOB_HEADER_SIZE) ||
                !read_exact(input, blank_header, sizeof(blank_header),
                            "blank-audio length")) {
                scan_ok = false;
                break;
            }
            uint32_t silent_bytes = read_u32_le(blank_header);
            if (silent_bytes % 2 != 0 || UINT64_MAX - audio_bytes < silent_bytes) {
                fprintf(stderr, "Invalid blank-audio byte count at blob %" PRIu64 ".\n", i);
                scan_ok = false;
                break;
            }
            audio_bytes += silent_bytes;
            blank_count++;
        }
    }
    print_metadata(&container, (uint64_t)end, first_blob_size, first_blob_id);
    if (!scan_ok || (audio_count == 0 && blank_count == 0) || audio_bytes == 0) {
        if (scan_ok)
            fprintf(stderr, "No audio blobs were found in the container.\n");
        free(container.blob_offsets);
        fclose(input);
        return false;
    }

    bool pal = metadata_is_pal(input, &container);
    unsigned channels = 2;
    if (observed_count != 0) {
        qsort(observed_sizes, observed_count, sizeof(observed_sizes[0]), compare_u64);
        uint64_t median = observed_sizes[observed_count / 2];
        long double bytes_per_channel_field =
            pal ? 44100.0L * 2.0L / 50.0L
                : 44100.0L * 2.0L * 1001.0L / 60000.0L;
        unsigned estimate = (unsigned)((long double)median /
                                       bytes_per_channel_field + 0.5L);
        if (estimate == 0 || estimate > 8) {
            fprintf(stderr, "Could not infer a valid channel count from PCM blobs.\n");
            free(container.blob_offsets);
            fclose(input);
            return false;
        }
        channels = estimate;
    } else {
        fprintf(stderr, "Only blank audio was found; assuming stereo for channel layout.\n");
    }

    uint64_t block_align = (uint64_t)channels * 2;
    if (audio_bytes % block_align != 0) {
        fprintf(stderr, "Total PCM size is not aligned to %u-channel 16-bit samples.\n",
                channels);
        free(container.blob_offsets);
        fclose(input);
        return false;
    }
    char *stem = output_stem(arguments->input_path);
    char *output_path = stem == NULL ? NULL :
        append_suffix(stem, arguments->audio_flac ? ".flac" : ".wav");
    free(stem);
    if (output_path == NULL) {
        fprintf(stderr, "Not enough memory to construct audio output path.\n");
        free(container.blob_offsets);
        fclose(input);
        return false;
    }
    if (access(output_path, F_OK) == 0) {
        fprintf(stderr, "Audio output already exists: %s\n", output_path);
        free(output_path);
        free(container.blob_offsets);
        fclose(input);
        return false;
    }
    if (!arguments->audio_flac && audio_bytes > UINT32_MAX - 36) {
        fprintf(stderr, "PCM audio is too large for a standard WAV file; use --flac.\n");
        free(output_path);
        free(container.blob_offsets);
        fclose(input);
        return false;
    }

    FILE *output = NULL;
    pid_t encoder_pid = -1;
    char *temp_path = NULL;
    bool success = true;
    bool wav_created = false;
    struct sigaction ignore_pipe = {0};
    struct sigaction old_pipe;
    bool changed_sigpipe = false;
    if (arguments->audio_flac) {
        if (sigemptyset(&ignore_pipe.sa_mask) != 0) {
            fprintf(stderr, "Could not prepare SIGPIPE handling: %s\n", strerror(errno));
            success = false;
        } else {
            ignore_pipe.sa_handler = SIG_IGN;
            if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe) != 0) {
                fprintf(stderr, "Could not configure SIGPIPE handling: %s\n", strerror(errno));
                success = false;
            } else {
                changed_sigpipe = true;
                temp_path = reserve_temp_output(output_path);
                success = temp_path != NULL &&
                          start_ffmpeg_flac(temp_path, channels, &output, &encoder_pid);
            }
        }
    } else {
        int fd = open(output_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            fprintf(stderr, "Could not create audio output %s: %s\n",
                    output_path, strerror(errno));
            success = false;
        } else {
            wav_created = true;
            output = fdopen(fd, "wb");
            if (output == NULL) {
                fprintf(stderr, "Could not open WAV output stream: %s\n", strerror(errno));
                close(fd);
                success = false;
            } else {
                success = write_wav_header(output, audio_bytes, (uint16_t)channels);
            }
        }
    }

    if (success) {
        uint64_t processed = 0;
        for (uint64_t i = 0; i < container.blob_count; i++) {
            uint64_t payload_size;
            uint32_t blob_id;
            if (!read_blob_header(input, &container, i, &payload_size, &blob_id)) {
                success = false;
                break;
            }
            if (blob_id != BLOB_ID_AUDIO && blob_id != BLOB_ID_AUDIO_BLANK)
                continue;
            if (!write_audio_payload(output, input, &container, i,
                                     payload_size, blob_id)) {
                success = false;
                break;
            }
            processed++;
            if (processed % 10000 == 0 || processed == audio_count + blank_count) {
                printf("\rProcessed %" PRIu64 " / %" PRIu64 " audio blobs",
                       processed, audio_count + blank_count);
                fflush(stdout);
            }
        }
        printf("\n");
    }

    if (arguments->audio_flac && output != NULL) {
        if (!finish_ffmpeg_flac(output, encoder_pid))
            success = false;
        if (success)
            success = publish_temp_output(temp_path, output_path);
    } else if (!arguments->audio_flac && output != NULL) {
        if (fclose(output) != 0) {
            fprintf(stderr, "Could not finish WAV file: %s\n", strerror(errno));
            success = false;
        }
    }
    if (changed_sigpipe && sigaction(SIGPIPE, &old_pipe, NULL) != 0) {
        fprintf(stderr, "Could not restore SIGPIPE handling: %s\n", strerror(errno));
        success = false;
    }
    if (!success) {
        if (wav_created)
            unlink(output_path);
        if (temp_path != NULL)
            unlink(temp_path);
    } else {
        printf("Wrote %s (source PCM: %" PRIu64
               " bytes, 44.1 kHz, %u-channel, 16-bit%s).\n",
               output_path, audio_bytes, channels,
               arguments->audio_flac ? " encoded losslessly as FLAC" : "");
    }

    free(temp_path);
    free(output_path);
    free(container.blob_offsets);
    fclose(input);
    return success;
}

static bool
inspect_jpeg(const unsigned char *data, size_t size, struct jpeg_info *info)
{
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8)
        return false;

    memset(info, 0, sizeof(*info));
    size_t i = 2;
    while (i + 1 < size) {
        if (data[i] != 0xff)
            return false;
        while (i < size && data[i] == 0xff)
            i++;
        if (i >= size)
            return false;

        unsigned char marker = data[i++];
        if (marker == 0xd9)
            return true;
        if (marker == 0x00 || marker == 0xd8 ||
            (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01)
            continue;
        if (i + 2 > size)
            return false;

        uint16_t segment_size = (uint16_t)data[i] << 8 | data[i + 1];
        if (segment_size < 2 || segment_size > size - i)
            return false;
        const unsigned char *segment = data + i + 2;
        size_t segment_data_size = segment_size - 2;

        if (marker == 0xdb)
            info->has_dqt = true;
        else if (marker == 0xc4)
            info->has_dht = true;
        else if (marker >= 0xc0 && marker <= 0xcf &&
                 marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (segment_data_size < 5)
                return false;
            info->height = (uint16_t)segment[1] << 8 | segment[2];
            info->width = (uint16_t)segment[3] << 8 | segment[4];
        }

        i += segment_size;
        if (marker == 0xda)
            break;
    }
    return info->width != 0 && info->height != 0;
}

static bool
write_bytes(FILE *output, const unsigned char *data, size_t size)
{
    if (size == 0)
        return true;
    if (fwrite(data, 1, size, output) != size) {
        fprintf(stderr, "Could not write output JPEG: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static bool
copy_file(FILE *source, FILE *destination)
{
    unsigned char buffer[8192];
    size_t count;
    rewind(source);
    while ((count = fread(buffer, 1, sizeof(buffer), source)) != 0) {
        if (fwrite(buffer, 1, count, destination) != count) {
            fprintf(stderr, "Could not write blank-field JPEG: %s\n", strerror(errno));
            return false;
        }
    }
    if (ferror(source)) {
        fprintf(stderr, "Could not read blank-field template: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static bool
make_blank_jpeg(const char *path, uint16_t width, uint16_t height)
{
    char size_arg[32];
    snprintf(size_arg, sizeof(size_arg), "%ux%u", width, height);
    char color_arg[64];
    snprintf(color_arg, sizeof(color_arg), "color=c=black:s=%s", size_arg);
    char *const argv[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", color_arg, "-frames:v", "1",
        "-q:v", "2", "-y", (char *)path, NULL
    };

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "Could not start ffmpeg: %s\n", strerror(errno));
        return false;
    }
    if (child == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "Could not execute ffmpeg: %s\n", strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        fprintf(stderr, "Could not wait for ffmpeg: %s\n", strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ffmpeg could not create the blank-field JPEG.\n");
        return false;
    }
    return true;
}

static char *
output_stem(const char *input_path)
{
    size_t length = strlen(input_path);
    char *stem = malloc(length + 1);
    if (stem == NULL)
        return NULL;
    memcpy(stem, input_path, length + 1);

    char *slash = strrchr(stem, '/');
    char *dot = strrchr(stem, '.');
    if (dot != NULL && (slash == NULL || dot > slash) && dot != (slash ? slash + 1 : stem))
        *dot = '\0';
    return stem;
}

static char *
append_suffix(const char *stem, const char *suffix)
{
    size_t length = strlen(stem) + strlen(suffix) + 1;
    char *path = malloc(length);
    if (path != NULL)
        snprintf(path, length, "%s%s", stem, suffix);
    return path;
}

static bool
wait_for_child(pid_t child, const char *operation)
{
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        fprintf(stderr, "Could not wait for %s: %s\n", operation, strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s failed.\n", operation);
        return false;
    }
    return true;
}

static char *
reserve_temp_output(const char *output_path)
{
    size_t length = strlen(output_path) + sizeof(".tmp.XXXXXX");
    char *temp_path = malloc(length);
    if (temp_path == NULL) {
        fprintf(stderr, "Not enough memory to reserve temporary output path.\n");
        return NULL;
    }
    snprintf(temp_path, length, "%s.tmp.XXXXXX", output_path);
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        fprintf(stderr, "Could not reserve temporary output near %s: %s\n",
                output_path, strerror(errno));
        free(temp_path);
        return NULL;
    }
    if (fchmod(fd, 0644) != 0) {
        fprintf(stderr, "Could not set temporary output permissions: %s\n",
                strerror(errno));
        close(fd);
        unlink(temp_path);
        free(temp_path);
        return NULL;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "Could not close temporary output reservation: %s\n",
                strerror(errno));
        unlink(temp_path);
        free(temp_path);
        return NULL;
    }
    return temp_path;
}

static bool
publish_temp_output(const char *temp_path, const char *output_path)
{
    if (link(temp_path, output_path) != 0) {
        fprintf(stderr, "Could not publish output %s: %s\n",
                output_path, strerror(errno));
        return false;
    }
    if (unlink(temp_path) != 0) {
        fprintf(stderr, "Could not remove temporary output %s: %s\n",
                temp_path, strerror(errno));
        return false;
    }
    return true;
}

static bool
merge_audio_video(const char *input_path)
{
    char *stem = output_stem(input_path);
    char *avi_path = stem == NULL ? NULL : append_suffix(stem, ".avi");
    char *wav_path = stem == NULL ? NULL : append_suffix(stem, ".wav");
    char *merged_path = stem == NULL ? NULL : append_suffix(stem, "_merged.avi");
    free(stem);
    if (avi_path == NULL || wav_path == NULL || merged_path == NULL) {
        fprintf(stderr, "Not enough memory to construct merge paths.\n");
        free(avi_path);
        free(wav_path);
        free(merged_path);
        return false;
    }
    if (access(avi_path, R_OK) != 0) {
        fprintf(stderr, "Could not read extracted video AVI %s: %s\n",
                avi_path, strerror(errno));
        free(avi_path);
        free(wav_path);
        free(merged_path);
        return false;
    }
    if (access(wav_path, R_OK) != 0) {
        fprintf(stderr, "Could not read extracted audio WAV %s: %s\n",
                wav_path, strerror(errno));
        free(avi_path);
        free(wav_path);
        free(merged_path);
        return false;
    }
    if (access(merged_path, F_OK) == 0) {
        fprintf(stderr, "Merged AVI already exists: %s\n", merged_path);
        free(avi_path);
        free(wav_path);
        free(merged_path);
        return false;
    }

    char *temp_path = reserve_temp_output(merged_path);
    if (temp_path == NULL) {
        free(avi_path);
        free(wav_path);
        free(merged_path);
        return false;
    }
    char *const argv[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin", "-y",
        "-i", avi_path, "-i", wav_path,
        "-map", "0:v:0", "-map", "1:a:0",
        "-c:v", "copy", "-c:a", "copy", "-f", "avi",
        temp_path, NULL
    };
    printf("Muxing %s and %s into %s.\n", avi_path, wav_path, merged_path);
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "Could not start ffmpeg: %s\n", strerror(errno));
        unlink(temp_path);
        free(temp_path);
        free(avi_path);
        free(wav_path);
        free(merged_path);
        return false;
    }
    if (child == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "Could not execute ffmpeg: %s\n", strerror(errno));
        _exit(127);
    }
    bool success = wait_for_child(child, "Audio/video mux");
    if (success)
        success = publish_temp_output(temp_path, merged_path);
    if (!success)
        unlink(temp_path);
    else
        printf("Wrote merged AVI: %s\n", merged_path);
    free(temp_path);
    free(avi_path);
    free(wav_path);
    free(merged_path);
    return success;
}

static bool
copy_blob_to_file(FILE *input, FILE *output, uint64_t payload_size)
{
    unsigned char buffer[65536];
    while (payload_size != 0) {
        size_t count = payload_size < sizeof(buffer)
                           ? (size_t)payload_size : sizeof(buffer);
        if (!read_exact(input, buffer, count, "VBI blob") ||
            fwrite(buffer, 1, count, output) != count) {
            if (ferror(output))
                fprintf(stderr, "Could not write VBI output: %s\n", strerror(errno));
            return false;
        }
        payload_size -= count;
    }
    return true;
}

#define VBI_SOURCE_RECORD_BYTES 10
#define VBI_MAME_RECORD_BYTES 16
#define VBI_UNPARSED_LINE 0x7fffffu
#define VBI_MASK_CAV_PICTURE 0xf00000u
#define VBI_CODE_CAV_PICTURE 0xf00000u

static uint32_t
get_u24be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void
put_u24be(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 16);
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)value;
}

static bool
vbi_picture_is_invalid_bcd(uint32_t value)
{
    return (value & 0xf000) > 0x9000 || (value & 0xf00) > 0x900 ||
           (value & 0xf0) > 0x90 || (value & 0xf) > 0x9;
}

/* Mirrors the line 17/18 selection in MAME's vbi_parse_all(). MAME's bit-confidence
   tiebreak needs raw signal data that LDIMG does not contain, so line 17 wins instead. */
static uint32_t
vbi_pick_line1718(uint32_t line17, uint32_t line18)
{
    if (line17 == 0)
        return line18;
    if (line18 == 0 || line17 == line18)
        return line17;
    if ((line17 & VBI_MASK_CAV_PICTURE) == VBI_CODE_CAV_PICTURE &&
        (line18 & VBI_MASK_CAV_PICTURE) == VBI_CODE_CAV_PICTURE) {
        if (vbi_picture_is_invalid_bcd(line17))
            return line18;
        if (vbi_picture_is_invalid_bcd(line18))
            return line17;
    }
    return line17;
}

/* Converts one 10-byte LDIMG record into MAME's 16-byte vbi_metadata_pack() layout. */
static void
vbi_convert_record(unsigned char *dest, uint32_t field_index, const unsigned char *source)
{
    uint32_t line[3];
    for (int i = 0; i < 3; i++) {
        line[i] = get_u24be(source + 1 + 3 * i);
        if (line[i] == VBI_UNPARSED_LINE)
            line[i] = 0;
    }
    put_u24be(dest, field_index);
    dest[3] = source[0] & 1;
    put_u24be(dest + 4, line[0]);
    put_u24be(dest + 7, line[1]);
    put_u24be(dest + 10, line[2]);
    put_u24be(dest + 13, vbi_pick_line1718(line[1], line[2]));
}

static bool
convert_vbi_blob(FILE *input, FILE *output, uint64_t records)
{
    unsigned char in[VBI_SOURCE_RECORD_BYTES * 4096];
    unsigned char out[VBI_MAME_RECORD_BYTES * 4096];
    uint64_t index = 0;
    while (index < records) {
        size_t count = records - index < 4096 ? (size_t)(records - index) : 4096;
        if (!read_exact(input, in, count * VBI_SOURCE_RECORD_BYTES, "VBI records"))
            return false;
        for (size_t i = 0; i < count; i++)
            vbi_convert_record(out + i * VBI_MAME_RECORD_BYTES, (uint32_t)(index + i),
                               in + i * VBI_SOURCE_RECORD_BYTES);
        if (fwrite(out, VBI_MAME_RECORD_BYTES, count, output) != count) {
            fprintf(stderr, "Could not write VBI output: %s\n", strerror(errno));
            return false;
        }
        index += count;
    }
    return true;
}

static bool
export_vbi(const struct arguments *arguments)
{
    FILE *input = fopen(arguments->input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Could not open input file %s: %s\n",
                arguments->input_path, strerror(errno));
        return false;
    }
    if (fseeko(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "Could not determine input file size: %s\n", strerror(errno));
        fclose(input);
        return false;
    }
    off_t end = ftello(input);
    if (end < 0) {
        fprintf(stderr, "Could not determine input file size: %s\n", strerror(errno));
        fclose(input);
        return false;
    }

    struct container container = {0};
    if (!load_container(input, (uint64_t)end, &container)) {
        free(container.blob_offsets);
        fclose(input);
        return false;
    }
    uint64_t vbi_index = UINT64_MAX;
    uint64_t vbi_size = 0;
    uint64_t video_fields = 0;
    uint64_t first_blob_size = 0;
    uint32_t first_blob_id = 0;
    for (uint64_t i = 0; i < container.blob_count; i++) {
        uint64_t payload_size;
        uint32_t blob_id;
        if (!read_blob_header(input, &container, i, &payload_size, &blob_id)) {
            free(container.blob_offsets);
            fclose(input);
            return false;
        }
        if (i == 0) {
            first_blob_size = payload_size;
            first_blob_id = blob_id;
        }
        if (blob_id == BLOB_ID_VBI) {
            if (vbi_index != UINT64_MAX) {
                fprintf(stderr, "Container has more than one VBI blob.\n");
                free(container.blob_offsets);
                fclose(input);
                return false;
            }
            vbi_index = i;
            vbi_size = payload_size;
        } else if (blob_id == BLOB_ID_VIDEO || blob_id == BLOB_ID_VIDEO_BLANK) {
            video_fields++;
        }
    }
    print_metadata(&container, (uint64_t)end, first_blob_size, first_blob_id);
    if (vbi_index == UINT64_MAX || vbi_size == 0) {
        fprintf(stderr, "No VBI blob is present in the LDIMG file.\n");
        free(container.blob_offsets);
        fclose(input);
        return false;
    }

    if (arguments->vbi_mame) {
        unsigned char header[4];
        if (vbi_size < sizeof(header) + 10 ||
            (vbi_size - sizeof(header)) % 10 != 0 ||
            !seek_to(input, container.blob_offsets[vbi_index] + BLOB_HEADER_SIZE) ||
            !read_exact(input, header, sizeof(header), "VBI file header") ||
            memcmp(header, "1VBI", sizeof(header)) != 0) {
            fprintf(stderr, "VBI blob is not in the expected LDIMG 1VBI 10-byte-per-field format.\n");
            free(container.blob_offsets);
            fclose(input);
            return false;
        }
        uint64_t record_count = (vbi_size - sizeof(header)) / 10;
        if (record_count != video_fields) {
            fprintf(stderr, "VBI contains %" PRIu64 " field records, but the LDIMG has "
                            "%" PRIu64 " video fields.\n", record_count, video_fields);
            free(container.blob_offsets);
            fclose(input);
            return false;
        }
    }

    char *stem = output_stem(arguments->input_path);
    char *output_path = stem == NULL ? NULL :
        append_suffix(stem, arguments->vbi_mame ? ".vbi" : ".vbi.raw");
    free(stem);
    if (output_path == NULL) {
        fprintf(stderr, "Not enough memory to construct VBI output path.\n");
        free(container.blob_offsets);
        fclose(input);
        return false;
    }
    int fd = open(output_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        fprintf(stderr, "Could not create VBI output %s: %s\n",
                output_path, strerror(errno));
        free(output_path);
        free(container.blob_offsets);
        fclose(input);
        return false;
    }
    FILE *output = fdopen(fd, "wb");
    if (output == NULL) {
        fprintf(stderr, "Could not open VBI output stream: %s\n", strerror(errno));
        close(fd);
        unlink(output_path);
        free(output_path);
        free(container.blob_offsets);
        fclose(input);
        return false;
    }
    bool success = seek_to(input, container.blob_offsets[vbi_index] + BLOB_HEADER_SIZE);
    if (success && arguments->vbi_mame) {
        unsigned char header[4];
        success = read_exact(input, header, sizeof(header), "VBI file header") &&
                  convert_vbi_blob(input, output, video_fields);
    } else if (success) {
        success = copy_blob_to_file(input, output, vbi_size);
    }
    if (fclose(output) != 0) {
        fprintf(stderr, "Could not finish VBI output: %s\n", strerror(errno));
        success = false;
    }
    if (!success) {
        unlink(output_path);
    } else {
        if (arguments->vbi_mame)
            printf("Wrote %s (%" PRIu64 " bytes): %" PRIu64
                   " MAME 16-byte field records.\n", output_path,
                   video_fields * VBI_MAME_RECORD_BYTES, video_fields);
        else
            printf("Wrote %s (%" PRIu64 " bytes); original blob payload copied "
                   "byte-for-byte.\n", output_path, vbi_size);
    }
    free(output_path);
    free(container.blob_offsets);
    fclose(input);
    return success;
}

static bool
run_ffmpeg_avi(const char *frames_dir, const char *avi_path,
               uint64_t field_count, uint16_t width, uint16_t field_height)
{
    char *pattern = append_suffix(frames_dir, "/batch_*/field_*.jpg");
    if (pattern == NULL) {
        fprintf(stderr, "Not enough memory to construct ffmpeg arguments.\n");
        return false;
    }

    const char *field_rate = field_height == 288 ? "50" : "60000/1001";
    const char *frame_rate = field_height == 288 ? "25" : "30000/1001";
    char *const argv[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "warning", "-n",
        "-pattern_type", "glob", "-framerate", (char *)field_rate,
        "-i", pattern,
        "-vf", "weave=first_field=top",
        "-c:v", "rawvideo", "-pix_fmt", "bgr24", "-an",
        "-r", (char *)frame_rate, "-f", "avi", (char *)avi_path, NULL
    };

    printf("Combining %" PRIu64 " fields into %s (%ux%u, uncompressed BGR24).\n",
           field_count, avi_path, width, field_height * 2);

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "Could not start ffmpeg: %s\n", strerror(errno));
        free(pattern);
        return false;
    }
    if (child == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "Could not execute ffmpeg: %s\n", strerror(errno));
        _exit(127);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        fprintf(stderr, "Could not wait for ffmpeg: %s\n", strerror(errno));
        free(pattern);
        return false;
    }
    free(pattern);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ffmpeg failed to create the AVI.\n");
        return false;
    }
    return true;
}

static bool
confirm_proceed(const char *action)
{
    char line[64];
    for (;;) {
        printf("%s Proceed? [Y/N] ", action);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\nNo answer received; nothing was done.\n");
            return false;
        }
        bool complete = strchr(line, '\n') != NULL;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        bool terminated = p[1] == '\n' || p[1] == '\0' || p[1] == ' ';
        if ((p[0] == 'y' || p[0] == 'Y') && terminated)
            return true;
        if ((p[0] == 'n' || p[0] == 'N') && terminated)
            return false;
        if (!complete) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF)
                ;
        }
        printf("Please answer Y or N.\n");
    }
}

/* Returns 1 if the directory holds exactly the expected batch layout, 0 if not, -1 if absent. */
static int
frames_dir_matches(const char *frames_dir, uint64_t field_count, uint64_t batch_size,
                   uint64_t *found_out)
{
    *found_out = 0;
    DIR *dir = opendir(frames_dir);
    if (dir == NULL)
        return errno == ENOENT ? -1 : 0;
    uint64_t expected_batches = (field_count + batch_size - 1) / batch_size;
    uint64_t found = 0;
    bool layout_ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        uint64_t batch;
        int consumed = 0;
        if (sscanf(name, "batch_%" SCNu64 "%n", &batch, &consumed) != 1 ||
            name[consumed] != '\0' || batch >= expected_batches) {
            layout_ok = false;
            continue;
        }
        char *prefix = append_suffix(frames_dir, "/");
        char *full = prefix == NULL ? NULL : append_suffix(prefix, name);
        free(prefix);
        DIR *sub = full == NULL ? NULL : opendir(full);
        free(full);
        if (sub == NULL) {
            layout_ok = false;
            continue;
        }
        uint64_t count = 0;
        struct dirent *file;
        while ((file = readdir(sub)) != NULL) {
            uint64_t index;
            int used = 0;
            if (sscanf(file->d_name, "field_%" SCNu64 ".jpg%n", &index, &used) == 1 &&
                file->d_name[used] == '\0')
                count++;
        }
        closedir(sub);
        uint64_t expected = batch + 1 == expected_batches
                          ? field_count - batch * batch_size : batch_size;
        if (count != expected)
            layout_ok = false;
        found += count;
    }
    closedir(dir);
    *found_out = found;
    return layout_ok && found == field_count ? 1 : 0;
}

static int
extract_video(const struct arguments *arguments)
{
    FILE *input = fopen(arguments->input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Could not open input file %s: %s\n",
                arguments->input_path, strerror(errno));
        return EXIT_FAILURE;
    }
    if (fseeko(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "Could not determine input file size: %s\n", strerror(errno));
        fclose(input);
        return EXIT_FAILURE;
    }
    off_t end = ftello(input);
    if (end < 0) {
        fprintf(stderr, "Could not determine input file size: %s\n", strerror(errno));
        fclose(input);
        return EXIT_FAILURE;
    }

    struct container container = {0};
    if (!load_container(input, (uint64_t)end, &container)) {
        free(container.blob_offsets);
        fclose(input);
        return EXIT_FAILURE;
    }

    uint64_t tables_index = UINT64_MAX;
    uint64_t first_video_index = UINT64_MAX;
    uint64_t field_count = 0;
    uint64_t first_blob_size = 0;
    uint32_t first_blob_id = 0;
    for (uint64_t i = 0; i < container.blob_count; i++) {
        uint64_t payload_size;
        uint32_t blob_id;
        if (!read_blob_header(input, &container, i, &payload_size, &blob_id)) {
            free(container.blob_offsets);
            fclose(input);
            return EXIT_FAILURE;
        }
        if (i == 0) {
            first_blob_size = payload_size;
            first_blob_id = blob_id;
        }
        if (blob_id == BLOB_ID_JPEG_TABLES)
            tables_index = i;
        if (blob_id == BLOB_ID_VIDEO || blob_id == BLOB_ID_VIDEO_BLANK) {
            if (first_video_index == UINT64_MAX && blob_id == BLOB_ID_VIDEO)
                first_video_index = i;
            field_count++;
        }
    }
    print_metadata(&container, (uint64_t)end, first_blob_size, first_blob_id);
    if (first_video_index == UINT64_MAX || field_count == 0) {
        fprintf(stderr, "No JPEG video fields were found in the container.\n");
        free(container.blob_offsets);
        fclose(input);
        return EXIT_FAILURE;
    }
    if (arguments->make_avi && field_count % 2 != 0) {
        fprintf(stderr, "Found an odd number of video fields (%" PRIu64
                        "); cannot pair all fields into AVI frames.\n", field_count);
        free(container.blob_offsets);
        fclose(input);
        return EXIT_FAILURE;
    }

    unsigned char *tables = NULL;
    size_t tables_size = 0;
    if (tables_index != UINT64_MAX) {
        uint64_t blob_size;
        uint32_t blob_id;
        if (!read_blob_header(input, &container, tables_index, &blob_size, &blob_id) ||
            blob_size > SIZE_MAX ||
            !read_blob_payload(input, &container, tables_index, blob_size, &tables)) {
            free(container.blob_offsets);
            fclose(input);
            return EXIT_FAILURE;
        }
        tables_size = (size_t)blob_size;
        if (tables_size < 4 || tables[0] != 0xff || tables[1] != 0xd8) {
            fprintf(stderr, "JPEG tables blob is malformed.\n");
            free(tables);
            free(container.blob_offsets);
            fclose(input);
            return EXIT_FAILURE;
        }
    }

    uint64_t first_size;
    uint32_t first_id;
    unsigned char *first_data = NULL;
    struct jpeg_info first_info;
    if (!read_blob_header(input, &container, first_video_index, &first_size, &first_id) ||
        !read_blob_payload(input, &container, first_video_index, first_size, &first_data) ||
        !inspect_jpeg(first_data, (size_t)first_size, &first_info)) {
        fprintf(stderr, "Could not read JPEG dimensions from the first video field.\n");
        free(first_data);
        free(tables);
        free(container.blob_offsets);
        fclose(input);
        return EXIT_FAILURE;
    }
    if ((!first_info.has_dqt || !first_info.has_dht) && tables == NULL) {
        fprintf(stderr, "The first video field needs JPEG tables, but no tables blob exists.\n");
        free(first_data);
        free(container.blob_offsets);
        fclose(input);
        return EXIT_FAILURE;
    }
    free(first_data);

    if (arguments->make_avi) {
        long double estimated = (long double)(field_count / 2) *
                                first_info.width * (first_info.height * 2) * 3;
        printf("Estimated uncompressed AVI video payload: %.1Lf GiB.\n",
               estimated / (1024.0L * 1024.0L * 1024.0L));
    }

    char *stem = output_stem(arguments->input_path);
    char *frames_dir = stem == NULL ? NULL : append_suffix(stem, ".frames");
    char *avi_path = stem == NULL ? NULL : append_suffix(stem, ".avi");
    char *blank_path = frames_dir == NULL ? NULL : append_suffix(frames_dir, "/blank-template.jpg");
    if (stem == NULL || frames_dir == NULL || avi_path == NULL || blank_path == NULL) {
        fprintf(stderr, "Not enough memory to construct output paths.\n");
        free(stem);
        free(frames_dir);
        free(avi_path);
        free(blank_path);
        free(tables);
        free(container.blob_offsets);
        fclose(input);
        return EXIT_FAILURE;
    }
    if (arguments->make_avi && access(avi_path, F_OK) == 0) {
        fprintf(stderr, "AVI output already exists: %s\n", avi_path);
        goto failure;
    }
    uint64_t existing_fields;
    int reuse_state = frames_dir_matches(frames_dir, field_count,
                                         arguments->batch_size, &existing_fields);
    if (reuse_state == 0) {
        fprintf(stderr, "Frames directory %s exists but does not match this LDIMG "
                        "(found %" PRIu64 " fields, expected %" PRIu64 " with batch size %"
                        PRIu64 ").\nRemove it or use a matching --batch-size.\n",
                frames_dir, existing_fields, field_count, arguments->batch_size);
        goto failure;
    }
    bool reuse = reuse_state == 1;
    if (reuse && !arguments->make_avi) {
        printf("Frames directory %s already contains all %" PRIu64
               " fields; nothing to do.\n", frames_dir, field_count);
        free(stem);
        free(frames_dir);
        free(avi_path);
        free(blank_path);
        free(tables);
        free(container.blob_offsets);
        fclose(input);
        return EXIT_SUCCESS;
    }
    if (reuse)
        printf("Existing frames directory %s has all %" PRIu64
               " fields; extraction will be skipped.\n", frames_dir, field_count);
    if (!confirm_proceed(reuse ? "This will encode the existing frames to an AVI."
                         : arguments->make_avi
                           ? "This will extract the frames and encode them to an AVI."
                           : "This will extract the frames.")) {
        printf("Cancelled.\n");
        free(stem);
        free(frames_dir);
        free(avi_path);
        free(blank_path);
        free(tables);
        free(container.blob_offsets);
        fclose(input);
        return EXIT_SUCCESS;
    }
    if (reuse)
        goto encode;
    if (mkdir(frames_dir, 0755) != 0) {
        fprintf(stderr, "Could not create frames directory %s: %s\n",
                frames_dir, strerror(errno));
        goto failure;
    }
    if (!make_blank_jpeg(blank_path, first_info.width, first_info.height))
        goto failure;
    FILE *blank = fopen(blank_path, "rb");
    if (blank == NULL) {
        fprintf(stderr, "Could not open generated blank-field JPEG: %s\n", strerror(errno));
        goto failure;
    }

    printf("Extracting %" PRIu64 " fields from %s to %s.\n",
           field_count, arguments->input_path, frames_dir);
    uint64_t field_number = 0;
    uint64_t current_batch = UINT64_MAX;
    for (uint64_t i = 0; i < container.blob_count; i++) {
        uint64_t payload_size;
        uint32_t blob_id;
        if (!read_blob_header(input, &container, i, &payload_size, &blob_id)) {
            fclose(blank);
            goto failure;
        }
        if (blob_id != BLOB_ID_VIDEO && blob_id != BLOB_ID_VIDEO_BLANK)
            continue;

        uint64_t batch_number = field_number / arguments->batch_size;
        if (batch_number != current_batch) {
            char batch_name[48];
            snprintf(batch_name, sizeof(batch_name), "/batch_%06" PRIu64,
                     batch_number);
            char *batch_path = append_suffix(frames_dir, batch_name);
            if (batch_path == NULL) {
                fprintf(stderr, "Not enough memory to construct a batch directory.\n");
                fclose(blank);
                goto failure;
            }
            if (mkdir(batch_path, 0755) != 0) {
                fprintf(stderr, "Could not create batch directory %s: %s\n",
                        batch_path, strerror(errno));
                free(batch_path);
                fclose(blank);
                goto failure;
            }
            free(batch_path);
            current_batch = batch_number;
        }

        char filename[96];
        snprintf(filename, sizeof(filename),
                 "/batch_%06" PRIu64 "/field_%04" PRIu64 ".jpg",
                 batch_number, field_number % arguments->batch_size);
        char *output_path = append_suffix(frames_dir, filename);
        if (output_path == NULL) {
            fprintf(stderr, "Not enough memory to construct a field filename.\n");
            fclose(blank);
            goto failure;
        }
        FILE *output = fopen(output_path, "wb");
        if (output == NULL) {
            fprintf(stderr, "Could not create %s: %s\n", output_path, strerror(errno));
            free(output_path);
            fclose(blank);
            goto failure;
        }

        bool ok;
        if (blob_id == BLOB_ID_VIDEO_BLANK) {
            ok = copy_file(blank, output);
        } else {
            unsigned char *data = NULL;
            struct jpeg_info info;
            ok = read_blob_payload(input, &container, i, payload_size, &data);
            if (ok && (!inspect_jpeg(data, (size_t)payload_size, &info) ||
                       info.width != first_info.width || info.height != first_info.height)) {
                fprintf(stderr, "Invalid or inconsistent JPEG field at blob %" PRIu64 ".\n", i);
                ok = false;
            }
            bool needs_tables = ok && (!info.has_dqt || !info.has_dht);
            if (ok && needs_tables && tables == NULL) {
                fprintf(stderr, "JPEG field at blob %" PRIu64 " is missing its tables.\n", i);
                ok = false;
            }
            if (ok && needs_tables) {
                size_t prefix = tables_size;
                if (prefix >= 2 && tables[prefix - 2] == 0xff &&
                    tables[prefix - 1] == 0xd9)
                    prefix -= 2;
                size_t frame_offset = payload_size >= 2 &&
                                      data[0] == 0xff && data[1] == 0xd8 ? 2 : 0;
                ok = write_bytes(output, tables, prefix) &&
                     write_bytes(output, data + frame_offset,
                                 (size_t)payload_size - frame_offset);
            } else if (ok) {
                ok = write_bytes(output, data, (size_t)payload_size);
            }
            free(data);
        }
        if (fclose(output) != 0) {
            fprintf(stderr, "Could not finish writing %s: %s\n",
                    output_path, strerror(errno));
            ok = false;
        }
        free(output_path);
        if (!ok) {
            fclose(blank);
            goto failure;
        }
        field_number++;
        if (field_number % 5000 == 0 || field_number == field_count) {
            printf("\rExtracted %" PRIu64 " / %" PRIu64 " fields",
                   field_number, field_count);
            fflush(stdout);
        }
    }
    printf("\n");
    fclose(blank);
    unlink(blank_path);

encode:;
    bool success = true;
    if (arguments->make_avi)
        success = run_ffmpeg_avi(frames_dir, avi_path, field_count,
                                 first_info.width, first_info.height);
    if (!success)
        fprintf(stderr, "JPEG fields are available in %s.\n", frames_dir);

    free(stem);
    free(frames_dir);
    free(avi_path);
    free(blank_path);
    free(tables);
    free(container.blob_offsets);
    fclose(input);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;

failure:
    free(stem);
    free(frames_dir);
    free(avi_path);
    free(blank_path);
    free(tables);
    free(container.blob_offsets);
    fclose(input);
    return EXIT_FAILURE;
}

int
main(int argc, char *argv[])
{
    struct arguments arguments = {
        .input_path = NULL,
        .make_avi = true,
        .batch_size = 1000,
        .extract_audio = false,
        .audio_flac = false,
        .merge_av = false,
        .export_vbi = false,
        .vbi_mame = false
    };
    argp_parse(&argp, argc, argv, 0, 0, &arguments);
    if (arguments.merge_av)
        return merge_audio_video(arguments.input_path) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (arguments.export_vbi)
        return export_vbi(&arguments) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (arguments.extract_audio)
        return extract_audio(&arguments) ? EXIT_SUCCESS : EXIT_FAILURE;
    return extract_video(&arguments);
}
