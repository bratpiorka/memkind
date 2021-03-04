# SPDX-License-Identifier: BSD-2-Clause
# Copyright (C) 2021 Intel Corporation.

import re

from python_framework import CMD_helper


class Test_tiering_log_level(object):
    # NOTE: this script should be called from the root of memkind repository
    ld_preload_env = "LD_PRELOAD=tiering/.libs/libmemtier.so"
    # TODO create separate, parametrized binary that could be used for testing
    # instead of using ls here
    bin_path = "ls -l"
    cmd_helper = CMD_helper()

    log_msg_prefix = "MEMKIND_MEM_TIERING_LOG_DEBUG: "
    re_hex_or_nil = r"((0[xX][a-fA-F0-9]+)|\(nil\))"
    re_log_valid_messages = [
        log_msg_prefix + r"malloc\(\d+\) = " + re_hex_or_nil,
        log_msg_prefix +
        r"realloc\(" + re_hex_or_nil + r", \d+\) = " + re_hex_or_nil,
        log_msg_prefix + r"calloc\(\d+, \d+\) = " + re_hex_or_nil,
        log_msg_prefix + r"memalign\(\d+, \d +\) = " + re_hex_or_nil,
        log_msg_prefix + r"free\(" + re_hex_or_nil + r"\)",
        log_msg_prefix + r"kind_name: \w+",
        log_msg_prefix + r"pmem_path: .*",  # TODO add path re
        log_msg_prefix + r"pmem_size: (N/A)|\d+",
        log_msg_prefix + r"ratio_value: \d+",
    ]

    def get_ld_preload_cmd_output(self, config_env, log_level=None):
        log_level_env = "MEMKIND_MEM_TIERING_LOG_LEVEL=" + log_level \
            if log_level else ""
        command = " ".join([self.ld_preload_env, log_level_env,
                            config_env, self.bin_path])
        output, retcode = self.cmd_helper.execute_cmd(command)

        assert retcode == 0, \
            "Test failed with error: \nExecution of: '" + command + \
            "' returns: " + str(retcode) + "\noutput: " + output
        return output

    def get_default_cmd_output(self):
        output, retcode = self.cmd_helper.execute_cmd(self.bin_path)

        assert retcode == 0, \
            "Test failed with error: \nExecution of: '" + self.bin_path + \
            "' returns: " + str(retcode) + "\noutput: " + output
        return output

    def test_utils_init(self):
        default_output = self.get_default_cmd_output()
        default_output = default_output.split("\n")

        output = self.get_ld_preload_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=DRAM:1", log_level="1")
        output = output.split("\n")

        assert output[0] == "MEMKIND_MEM_TIERING_LOG_INFO: " + \
            "Memkind memtier lib loaded!", "Bad init message"

        # check if rest of output from command is unchanged
        rest_output = output[1:]
        assert rest_output == default_output, "Bad command output"

    def test_utils_log_level_default(self):
        default_output = self.get_default_cmd_output()
        default_output = default_output.split("\n")

        output = self.get_ld_preload_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=DRAM:1", log_level="0")
        output = output.split("\n")

        assert output == default_output, "Bad command output"

    def test_utils_log_level_not_set(self):
        default_output = self.get_default_cmd_output()
        default_output = default_output.split("\n")

        output = self.get_ld_preload_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=DRAM:1", log_level=None)
        output = output.split("\n")

        assert output == default_output, "Bad command output"

    def test_utils_log_level_debug(self):
        default_output = self.get_default_cmd_output()
        default_output = default_output.split("\n")

        output = self.get_ld_preload_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=DRAM:1", log_level="2")
        output = output.split("\n")

        assert output[0] == "MEMKIND_MEM_TIERING_LOG_DEBUG: " + \
            "Setting log level to: 2", "Bad init message"

        assert output[1] == "MEMKIND_MEM_TIERING_LOG_INFO: " + \
            "Memkind memtier lib loaded!", "Bad init message"

        # extract from the output all lines starting with
        # "MEMKIND_MEM_TIERING_LOG" prefix and check if they are correct
        log_output = [l for l in output[2:]
                      if l.startswith("MEMKIND_MEM_TIERING_LOG")]
        for log_line in log_output:
            log_line_valid = any(re.match(pattern, log_line)
                                 for pattern in self.re_log_valid_messages)
            assert log_line_valid, "Bad log message format: " + log_line

        # finally check if rest of output from command is unchanged
        output = [l for l in output if l not in log_output][2:]
        assert output == default_output, "Bad ls output"

    def test_utils_log_level_negative(self):
        log_level_env = "MEMKIND_MEM_TIERING_LOG_LEVEL=-1"
        command = " ".join([self.ld_preload_env, log_level_env, self.bin_path])
        output_level_neg, _ = self.cmd_helper.execute_cmd(command)

        assert output_level_neg.split("\n")[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Wrong value of " + \
            "MEMKIND_MEM_TIERING_LOG_LEVEL=-1", "Bad init message"

    def test_utils_log_level_too_high(self):
        log_level_env = "MEMKIND_MEM_TIERING_LOG_LEVEL=4"
        command = " ".join([self.ld_preload_env, log_level_env, self.bin_path])
        output_level_neg, _ = self.cmd_helper.execute_cmd(command)

        assert output_level_neg.split("\n")[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Wrong value of " + \
            "MEMKIND_MEM_TIERING_LOG_LEVEL=4", "Bad init message"


class Test_tiering_config_env(object):
    ld_preload_env = "LD_PRELOAD=tiering/.libs/libmemtier.so"
    cmd_helper = CMD_helper()

    def get_cmd_output(self, config_env, log_level="0"):
        command = " ".join(
            [self.ld_preload_env, "MEMKIND_MEM_TIERING_LOG_LEVEL=" + log_level, config_env, "ls"])
        output, retcode = self.cmd_helper.execute_cmd(command)

        assert retcode == 0, \
            "Test failed with error: \nExecution of: '" + command + \
            "' returns: " + str(retcode) + "\noutput: " + output
        return output

    def test_DRAM_only(self):
        output = self.get_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=DRAM:1", log_level="2")

        assert "MEMKIND_MEM_TIERING_LOG_DEBUG: kind_name: DRAM" in output.splitlines(), \
               "Wrong message"
        assert "MEMKIND_MEM_TIERING_LOG_DEBUG: ratio_value: 1" in output.splitlines(), \
               "Wrong message"

    def test_FSDAX_only(self):
        output = self.get_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=FS_DAX:/tmp/:10G:1", log_level="2")

        assert "MEMKIND_MEM_TIERING_LOG_DEBUG: kind_name: FS_DAX" in output.splitlines(), \
               "Wrong message"
        assert "MEMKIND_MEM_TIERING_LOG_DEBUG: pmem_path: /tmp/" in output.splitlines(), \
               "Wrong message"
        assert "MEMKIND_MEM_TIERING_LOG_DEBUG: pmem_size: 10G" in output.splitlines(), \
               "Wrong message"
        assert "MEMKIND_MEM_TIERING_LOG_DEBUG: ratio_value: 1" in output.splitlines(), \
               "Wrong message"

    def test_FSDAX_negative_size(self):
        output = self.get_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=FS_DAX:/tmp/:-1:1")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported pmem_size format: -1", "Wrong message"

    def test_FSDAX_negative_size_min(self):
        output = self.get_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=FS_DAX:/tmp/:-9223372036854775808:1")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported pmem_size format: -9223372036854775808", "Wrong message"

    def test_FSDAX_wrong_size(self):
        output = self.get_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=FS_DAX:/tmp/:as:1")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported pmem_size format: as", "Wrong message"

    def test_FSDAX_negative_ratio(self):
        output = self.get_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=FS_DAX:/tmp/:10G:-1")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported ratio: -1", "Wrong message"

    def test_FSDAX_wrong_ratio(self):
        output = self.get_cmd_output(
            "MEMKIND_MEM_TIERING_CONFIG=FS_DAX:/tmp/:10G:a")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported ratio: a", "Wrong message"

    def test_class_not_defined(self):
        output = self.get_cmd_output("MEMKIND_MEM_TIERING_CONFIG=2")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported kind: 2", "Wrong message"

    def test_bad_ratio(self):
        output = self.get_cmd_output("MEMKIND_MEM_TIERING_CONFIG=DRAM:A")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported ratio: A", "Wrong message"

    def test_no_ratio(self):
        output = self.get_cmd_output("MEMKIND_MEM_TIERING_CONFIG=DRAM")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Ratio not provided", "Wrong message"

    def test_bad_class(self):
        output = self.get_cmd_output("MEMKIND_MEM_TIERING_CONFIG=a1b2:10")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported kind: a1b2", "Wrong message"

    def test_negative_ratio(self):
        output = self.get_cmd_output("MEMKIND_MEM_TIERING_CONFIG=DRAM:-1")

        assert output.splitlines()[0] == \
            "MEMKIND_MEM_TIERING_LOG_ERROR: Unsupported ratio: -1", "Wrong message"
