/**
 * @file env_info.h
 * @brief Collects environment information (OS, cwd, local time) injected
 *        into the session context as part of the system message.
 */

#ifndef ENV_INFO_H
#define ENV_INFO_H

/**
 * @brief Builds the environment info block appended to the system prompt.
 *
 * The block describes the machine the agent runs on: operating system
 * (uname: sysname, release, machine), current working directory and the
 * local date/time. Fields that cannot be collected (e.g. a deleted
 * working directory) are omitted or reported as "(unavailable)"; a
 * partial block is still returned whenever memory allows, so collection
 * failures never abort the session startup.
 *
 * @return NUL-terminated heap string owned by the caller (free with
 *         free()), or NULL on allocation failure.
 */
char *env_info_build(void);

#endif /* ENV_INFO_H */
