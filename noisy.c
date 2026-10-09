#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>

#include <curl/curl.h>
#include <jansson.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_TIMEOUT_MS 5000L

enum log_level {
	LOG_DEBUG,
	LOG_INFO,
	LOG_WARNING,
	LOG_ERROR,
	LOG_CRITICAL
};

struct string_list {
	char	**items;
	size_t	 count;
	size_t	 capacity;
};

struct config {
	struct string_list	blacklisted_urls;
	struct string_list	root_urls;
	struct string_list	user_agents;
	long			max_depth;
	long			min_sleep;
	long			max_sleep;
	long			timeout;
	bool			timeout_set;
};

struct body {
	char	*data;
	size_t	length;
};

static volatile sig_atomic_t interrupted;
static enum log_level log_threshold = LOG_INFO;

static void	 usage(int);
static void	 log_message(enum log_level, const char *, ...);
static int	 list_append(struct string_list *, const char *);
static void	 list_free(struct string_list *);
static int	 load_config(const char *, struct config *);
static int	 valid_web_url(const char *);
static char	*normalize_url(const char *, const char *);
static int	 extract_urls(const struct config *, const char *, size_t,
		    const char *, struct string_list *);
static size_t	 write_body(char *, size_t, size_t, void *);
static int	 fetch_url(const char *, const char *, long,
		    struct body *, long *);
static int	 is_blacklisted(const struct config *, const char *);
static int	 should_accept_url(const struct config *, const char *);
static int	 mark_dead_link(struct config *, struct string_list *, size_t);
static int	 browse_links(struct config *, struct string_list *,
		    const struct timespec *);
static int	 crawl(struct config *);
static int	 parse_args(int, char **, char **, char **, long *,
		    enum log_level *);
static int	 parse_long(const char *, long *);
static long	 remaining_ms(const struct config *, const struct timespec *);
static void	 stop_signal(int);

static const char *
level_name(enum log_level level)
{
	switch (level) {
	case LOG_DEBUG:
		return "DEBUG";
	case LOG_INFO:
		return "INFO";
	case LOG_WARNING:
		return "WARNING";
	case LOG_ERROR:
		return "ERROR";
	case LOG_CRITICAL:
		return "CRITICAL";
	}
	return "INFO";
}

static void
log_message(enum log_level level, const char *format, ...)
{
	va_list ap;

	if (level < log_threshold)
		return;
	fprintf(stderr, "%s:", level_name(level));
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void
usage(int status)
{
	fprintf(status == 0 ? stdout : stderr,
	    "usage: noisy [-h] [-l level] -c config [-t seconds]\n");
	exit(status);
}

static int
list_append(struct string_list *list, const char *value)
{
	char **items;
	size_t capacity;

	if (list->count == list->capacity) {
		capacity = list->capacity == 0 ? 8 : list->capacity * 2;
		if (capacity < list->capacity ||
		    capacity > SIZE_MAX / sizeof(*list->items))
			return -1;
		items = realloc(list->items, capacity * sizeof(*list->items));
		if (items == NULL)
			return -1;
		list->items = items;
		list->capacity = capacity;
	}
	list->items[list->count] = strdup(value);
	if (list->items[list->count] == NULL)
		return -1;
	list->count++;
	return 0;
}

static void
list_free(struct string_list *list)
{
	size_t i;

	for (i = 0; i < list->count; i++)
		free(list->items[i]);
	free(list->items);
	memset(list, 0, sizeof(*list));
}

static int
config_integer(json_t *object, const char *key, long *value)
{
	json_t *item;
	json_int_t integer;

	item = json_object_get(object, key);
	if (!json_is_integer(item))
		return -1;
	integer = json_integer_value(item);
	if (integer < 0 || (uint64_t)integer > (uint64_t)LONG_MAX)
		return -1;
	*value = (long)integer;
	return 0;
}

static int
json_string_list(json_t *object, const char *key, struct string_list *list,
    bool required)
{
	json_t *array, *item;
	size_t index;

	array = json_object_get(object, key);
	if (!json_is_array(array))
		return -1;
	if (required && json_array_size(array) == 0)
		return -1;
	json_array_foreach(array, index, item) {
		if (!json_is_string(item) ||
		    list_append(list, json_string_value(item)) == -1)
			return -1;
	}
	return 0;
}

static int
load_config(const char *path, struct config *config)
{
	json_error_t error;
	json_t *root, *timeout;
	size_t i;

	root = json_load_file(path, 0, &error);
	if (root == NULL) {
		log_message(LOG_ERROR, "%s:%d: %s", path, error.line,
		    error.text);
		return -1;
	}
	if (!json_is_object(root) ||
	    config_integer(root, "max_depth", &config->max_depth) == -1 ||
	    config_integer(root, "min_sleep", &config->min_sleep) == -1 ||
	    config_integer(root, "max_sleep", &config->max_sleep) == -1 ||
	    config->max_sleep <= config->min_sleep ||
	    json_string_list(root, "root_urls", &config->root_urls, true) == -1 ||
	    json_string_list(root, "blacklisted_urls",
	    &config->blacklisted_urls, false) == -1 ||
	    json_string_list(root, "user_agents", &config->user_agents, true) == -1) {
		log_message(LOG_ERROR, "%s: invalid or missing configuration value",
		    path);
		json_decref(root);
		return -1;
	}
	for (i = 0; i < config->root_urls.count; i++) {
		if (!valid_web_url(config->root_urls.items[i])) {
			log_message(LOG_ERROR,
			    "%s: root_urls must contain only HTTP or HTTPS URLs",
			    path);
			json_decref(root);
			return -1;
		}
	}
	timeout = json_object_get(root, "timeout");
	if (json_is_false(timeout)) {
		config->timeout_set = false;
	} else if (json_is_integer(timeout) &&
	    config_integer(root, "timeout", &config->timeout) == 0) {
		config->timeout_set = true;
	} else {
		log_message(LOG_ERROR, "%s: timeout must be false or a nonnegative integer",
		    path);
		json_decref(root);
		return -1;
	}
	json_decref(root);
	return 0;
}

static int
valid_web_url(const char *value)
{
	CURLU *url;
	CURLUcode result;
	char *scheme = NULL;
	int valid = 0;

	url = curl_url();
	if (url == NULL)
		return 0;
	result = curl_url_set(url, CURLUPART_URL, value, 0);
	if (result == CURLUE_OK &&
	    curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
	    (strcasecmp(scheme, "http") == 0 ||
	    strcasecmp(scheme, "https") == 0))
		valid = 1;
	curl_free(scheme);
	curl_url_cleanup(url);
	return valid;
}

static char *
normalize_url(const char *link, const char *root_url)
{
	CURLU *url;
	CURLUcode result;
	char *normalized = NULL;

	url = curl_url();
	if (url == NULL)
		return NULL;
	result = curl_url_set(url, CURLUPART_URL, root_url, 0);
	if (result == CURLUE_OK)
		result = curl_url_set(url, CURLUPART_URL, link, 0);
	if (result == CURLUE_OK &&
	    curl_url_get(url, CURLUPART_URL, &normalized, 0) != CURLUE_OK)
		normalized = NULL;
	curl_url_cleanup(url);
	return normalized;
}

static int
is_blacklisted(const struct config *config, const char *url)
{
	size_t i;

	for (i = 0; i < config->blacklisted_urls.count; i++) {
		if (strstr(url, config->blacklisted_urls.items[i]) != NULL)
			return 1;
	}
	return 0;
}

static int
should_accept_url(const struct config *config, const char *url)
{
	return valid_web_url(url) && !is_blacklisted(config, url);
}

static int
extract_urls(const struct config *config, const char *body, size_t length,
	const char *root_url, struct string_list *links)
{
	size_t i, start, end;
	char quote, *reference, *normalized;

	for (i = 0; i + 6 <= length; i++) {
		if (memcmp(body + i, "href=", 5) != 0 ||
		    (body[i + 5] != '\'' && body[i + 5] != '"'))
			continue;
		quote = body[i + 5];
		start = i + 6;
		for (end = start; end < length && body[end] != quote; end++)
			;
		if (end == length)
			continue;
		if (end == start || body[start] == '#') {
			i = end;
			continue;
		}
		reference = malloc(end - start + 1);
		if (reference == NULL)
			return -1;
		memcpy(reference, body + start, end - start);
		reference[end - start] = '\0';
		normalized = normalize_url(reference, root_url);
		free(reference);
		if (normalized != NULL) {
			if (should_accept_url(config, normalized)) {
				if (list_append(links, normalized) == -1) {
					curl_free(normalized);
					return -1;
				}
			}
			curl_free(normalized);
		}
		i = end;
	}
	return 0;
}

static size_t
write_body(char *data, size_t size, size_t count, void *argument)
{
	struct body *body = argument;
	char *buffer;
	size_t bytes;

	if (size != 0 && count > SIZE_MAX / size)
		return 0;
	bytes = size * count;
	if (bytes > SIZE_MAX - body->length - 1)
		return 0;
	buffer = realloc(body->data, body->length + bytes + 1);
	if (buffer == NULL)
		return 0;
	body->data = buffer;
	memcpy(body->data + body->length, data, bytes);
	body->length += bytes;
	body->data[body->length] = '\0';
	return bytes;
}

static long
remaining_ms(const struct config *config, const struct timespec *start)
{
	struct timespec now;
	double elapsed, remaining;

	if (!config->timeout_set)
		return REQUEST_TIMEOUT_MS;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
		return 0;
	elapsed = (double)(now.tv_sec - start->tv_sec) +
	    (double)(now.tv_nsec - start->tv_nsec) / 1000000000.0;
	remaining = (double)config->timeout - elapsed;
	if (remaining <= 0)
		return 0;
	if (remaining >= (double)REQUEST_TIMEOUT_MS / 1000.0)
		return REQUEST_TIMEOUT_MS;
	return (long)(remaining * 1000.0);
}

static int
fetch_url(const char *url, const char *user_agent, long timeout_ms,
    struct body *body, long *status)
{
	CURL *curl;
	CURLcode result;

	curl = curl_easy_init();
	if (curl == NULL)
		return -1;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 30L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
	    (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
	    (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	result = curl_easy_perform(curl);
	if (result == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
	curl_easy_cleanup(curl);
	return result == CURLE_OK ? 0 : -1;
}

static int
mark_dead_link(struct config *config, struct string_list *links, size_t index)
{
	char *url;

	url = links->items[index];
	if (list_append(&config->blacklisted_urls, url) == -1)
		return -1;
	free(url);
	if (index + 1 < links->count)
		memmove(&links->items[index], &links->items[index + 1],
		    (links->count - index - 1) * sizeof(*links->items));
	links->count--;
	return 0;
}

static int
browse_links(struct config *config, struct string_list *links,
    const struct timespec *start)
{
	struct body body;
	struct string_list next_links;
	struct timespec request;
	unsigned long delay, span;
	long depth, left, status, timeout_ms;
	size_t index;
	int result;

	for (depth = 0; depth < config->max_depth &&
	    links->count > 0 && !interrupted; depth++) {
		timeout_ms = remaining_ms(config, start);
		if (timeout_ms == 0)
			return 1;
		index = (size_t)random() % links->count;
		log_message(LOG_INFO, "Visiting %s", links->items[index]);
		memset(&body, 0, sizeof(body));
		status = 0;
		result = fetch_url(links->items[index],
		    config->user_agents.items[(size_t)random() %
		    config->user_agents.count], timeout_ms, &body, &status);
		if (result == -1) {
			log_message(LOG_DEBUG,
			    "Request failed for %s; removing from links",
			    links->items[index]);
			free(body.data);
			if (mark_dead_link(config, links, index) == -1)
				return -1;
			continue;
		}
		log_message(LOG_DEBUG, "HTTP status %ld", status);
		memset(&next_links, 0, sizeof(next_links));
		if (extract_urls(config, body.data == NULL ? "" : body.data,
		    body.length, links->items[index], &next_links) == -1) {
			free(body.data);
			list_free(&next_links);
			return -1;
		}
		free(body.data);
		span = (unsigned long)(config->max_sleep - config->min_sleep);
		if (span > 0) {
			delay = (unsigned long)config->min_sleep +
			    (unsigned long)random() % span;
			request.tv_sec = (time_t)delay;
			request.tv_nsec = 0;

			if (config->timeout_set) {
				left = remaining_ms(config, start);
				if (left <= 0) {
					list_free(&next_links);
					return 1;
				}
				if ((double)delay * 1000.0 > (double)left) {
					request.tv_sec = left / 1000;
					request.tv_nsec = (left % 1000) * 1000000L;
				}
			}
			while (nanosleep(&request, &request) == -1 && errno == EINTR &&
			    !interrupted)
				;
		}
		if (next_links.count > 1) {
			list_free(links);
			*links = next_links;
		} else {
			list_free(&next_links);
			if (mark_dead_link(config, links, index) == -1)
				return -1;
		}
	}
	if (links->count == 0 || !interrupted)
		log_message(LOG_DEBUG, "Hit a dead end, moving to the next root URL");
	return 0;
}

static int
crawl(struct config *config)
{
	struct string_list links;
	struct body body;
	struct timespec start;
	long timeout_ms, status;
	int result;
	size_t index;

	if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
		log_message(LOG_ERROR, "clock_gettime: %s", strerror(errno));
		return -1;
	}
	while (!interrupted) {
		timeout_ms = remaining_ms(config, &start);
		if (timeout_ms == 0) {
			log_message(LOG_INFO, "Timeout has exceeded, exiting");
			break;
		}
		index = (size_t)random() % config->root_urls.count;
		memset(&body, 0, sizeof(body));
		status = 0;
		result = fetch_url(config->root_urls.items[index],
		    config->user_agents.items[(size_t)random() %
		    config->user_agents.count], timeout_ms, &body, &status);
		if (result == -1) {
			log_message(LOG_WARNING, "Error connecting to root URL: %s",
			    config->root_urls.items[index]);
			free(body.data);
			continue;
		}
		memset(&links, 0, sizeof(links));
		if (extract_urls(config, body.data == NULL ? "" : body.data,
		    body.length, config->root_urls.items[index], &links) == -1) {
			free(body.data);
			list_free(&links);
			return -1;
		}
		free(body.data);
		log_message(LOG_DEBUG, "Found %zu links", links.count);
		result = browse_links(config, &links, &start);
		list_free(&links);
		if (result == -1)
			return -1;
		if (result == 1) {
			log_message(LOG_INFO, "Timeout has exceeded, exiting");
			break;
		}
	}
	return 0;
}

static int
parse_long(const char *text, long *value)
{
	char *end;

	errno = 0;
	*value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;
	return 0;
}

static int
parse_log_level(const char *value, enum log_level *level)
{
	if (strcasecmp(value, "debug") == 0)
		*level = LOG_DEBUG;
	else if (strcasecmp(value, "info") == 0)
		*level = LOG_INFO;
	else if (strcasecmp(value, "warn") == 0 ||
	    strcasecmp(value, "warning") == 0)
		*level = LOG_WARNING;
	else if (strcasecmp(value, "error") == 0)
		*level = LOG_ERROR;
	else if (strcasecmp(value, "critical") == 0)
		*level = LOG_CRITICAL;
	else
		return -1;
	return 0;
}

static int
parse_args(int argc, char **argv, char **config_path, char **log_name,
    long *timeout, enum log_level *level)
{
	const char *argument, *value;
	int i;

	*config_path = NULL;
	*log_name = NULL;
	*timeout = 0;
	for (i = 1; i < argc; i++) {
		argument = argv[i];
		value = NULL;
		if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0)
			usage(0);
		if (strcmp(argument, "-c") == 0 ||
		    strcmp(argument, "--config") == 0) {
			if (++i >= argc)
				usage(2);
			*config_path = argv[i];
		} else if (strcmp(argument, "-l") == 0 ||
		    strcmp(argument, "--log") == 0) {
			if (++i >= argc)
				usage(2);
			*log_name = argv[i];
		} else if (strcmp(argument, "-t") == 0 ||
		    strcmp(argument, "--timeout") == 0) {
			if (++i >= argc || parse_long(argv[i], timeout) == -1)
				usage(2);
		} else if (strncmp(argument, "--config=", 9) == 0) {
			value = argument + 9;
			*config_path = (char *)value;
		} else if (strncmp(argument, "--log=", 6) == 0) {
			value = argument + 6;
			*log_name = (char *)value;
		} else if (strncmp(argument, "--timeout=", 10) == 0) {
			if (parse_long(argument + 10, timeout) == -1)
				usage(2);
		} else
			usage(2);
	}
	if (*config_path == NULL)
		usage(2);
	if (*log_name != NULL && parse_log_level(*log_name, level) == -1)
		usage(2);
	return 0;
}

static void
stop_signal(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

/*
 * Crawl randomly selected HTTP and HTTPS links from configured root URLs.
 */
int
main(int argc, char **argv)
{
	struct config config;
	struct sigaction action;
	char *config_path, *log_name;
	long timeout;
	enum log_level level = LOG_INFO;
	int result;

	memset(&config, 0, sizeof(config));
	memset(&action, 0, sizeof(action));
	if (parse_args(argc, argv, &config_path, &log_name, &timeout, &level) == -1)
		return 1;
	log_threshold = level;
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		log_message(LOG_ERROR, "Unable to initialize libcurl");
		return 1;
	}
	if (load_config(config_path, &config) == -1) {
		curl_global_cleanup();
		return 1;
	}
	if (timeout != 0) {
		config.timeout = timeout;
		config.timeout_set = true;
	}
	srandom((unsigned int)(time(NULL) ^ getpid()));
	action.sa_handler = stop_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	result = crawl(&config);
	curl_global_cleanup();
	list_free(&config.blacklisted_urls);
	list_free(&config.root_urls);
	list_free(&config.user_agents);
	return result == 0 ? 0 : 1;
}
