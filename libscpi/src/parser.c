/*-
 * Copyright (c) 2012-2013 Jan Breuer,
 *
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   scpi_parser.c
 * @date   Thu Nov 15 10:58:45 UTC 2012
 *
 * @brief  SCPI parser implementation
 *
 *
 */

#include <string.h>
#include <sys/socket.h>
#include <elf.h>
#include <stdio.h>

#include "scpi/config.h"
#include "scpi/parser.h"
#include "parser_private.h"
#include "lexer_private.h"
#include "scpi/error.h"
#include "scpi/constants.h"
#include "scpi/utils.h"

/**
 * Write data to SCPI output
 * @param context
 * @param data
 * @param len - lenght of data to be written
 * @return number of bytes written
 */
static size_t writeData(scpi_t * context, const char * data, size_t len) {
    return context->interface->write(context, data, len);
}

/**
 * Flush data to SCPI output
 * @param context
 * @return
 */
static int flushData(scpi_t * context) {
    if (context && context->interface && context->interface->flush) {
        return context->interface->flush(context);
    } else {
        return SCPI_RES_OK;
    }
}

/**
 * Write result delimiter to output
 * @param context
 * @return number of bytes written
 */
static size_t writeDelimiter(scpi_t * context) {
    if (context->output_count > 0) {
        return writeData(context, ",", 1);
    } else {
        return 0;
    }
}

/**
 * Conditionaly write "New Line"
 * @param context
 * @return number of characters written
 */
static size_t writeNewLine(scpi_t * context) {
    if (context->output_count > 0) {
        size_t len;
#ifndef SCPI_LINE_ENDING
#error no termination character defined
#endif
        len = writeData(context, SCPI_LINE_ENDING, strlen(SCPI_LINE_ENDING));
        flushData(context);
        return len;
    } else if (context->output_binary_count > 0) {
        flushData(context);
    }
    return 0;
}

/**
 * Writes header for binary data
 * @param context
 * @param numElems - number of items in the array
 * @param sizeOfElem - size of each item [sizeof(float), sizeof(int), ...]
 * @return number of characters written
 */
static size_t writeBinHeader(scpi_t * context, uint32_t numElems, size_t sizeOfElem) {

    size_t result = 0;
    char numBytes[9+1];
    char numOfNumBytes[2];

    // Calculate number of bytes needed for all elements
    size_t numDataBytes = numElems * sizeOfElem;

    // Do not allow more than 9 character long size
    if (numDataBytes > 999999999){
        return result;
    }

    // Convert to string and calculate string length
    size_t len = SCPI_UInt32ToStrBase(numDataBytes, numBytes, sizeof(numBytes), 10);

    // Convert len to sting
    SCPI_UInt32ToStrBase(len, numOfNumBytes, sizeof(numOfNumBytes), 10);

    result += writeData(context, "#", 1);
    result += writeData(context, numOfNumBytes, 1);
    result += writeData(context, numBytes, len);

    return result;
}


/**
 * Conditionaly write ";"
 * @param context
 * @return number of characters written
 */
static size_t writeSemicolon(scpi_t * context) {
    if (context->output_count > 0) {
        return writeData(context, ";", 1);
    } else {
        return 0;
    }
}

/**
 * Process command
 * @param context
 */
static scpi_bool_t processCommand(scpi_t * context) {
    const scpi_command_t * cmd = context->param_list.cmd;
    lex_state_t * state = &context->param_list.lex_state;
    scpi_bool_t result = TRUE;

    /* conditionaly write ; */
    writeSemicolon(context);

    context->cmd_error = FALSE;
    context->output_count = 0;
    context->output_binary_count = 0;
    context->input_count = 0;

    /* if callback exists - call command callback */
    if (cmd->callback != NULL) {
        if ((cmd->callback(context) != SCPI_RES_OK)) {
            if (!context->cmd_error) {
                SCPI_ErrorPush(context, SCPI_ERROR_EXECUTION_ERROR);
            }
            result = FALSE;
        } else {
            if (context->cmd_error) {
                result = FALSE;
            }
        }
    }

    /* set error if command callback did not read all parameters */
    if (state->pos < (state->buffer + state->len) && !context->cmd_error) {
        SCPI_ErrorPush(context, SCPI_ERROR_PARAMETER_NOT_ALLOWED);
        result = FALSE;
    }

    return result;
}

/**
 * Cycle all patterns and search matching pattern. Execute command callback.
 * @param context
 * @result TRUE if context->paramlist is filled with correct values
 */
static scpi_bool_t findCommandHeader(scpi_t * context, const char * header, int len) {
    int32_t i;
    const scpi_command_t * cmd;

    for (i = 0; context->cmdlist[i].pattern != NULL; i++) {
        cmd = &context->cmdlist[i];
        if (matchCommand(cmd->pattern, header, len, NULL, 0, 0)) {
            context->param_list.cmd = cmd;
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * Parse one command line
 * @param context
 * @param data - complete command line
 * @param len - command line length
 * @return FALSE if there was some error during evaluation of commands
 */
scpi_bool_t SCPI_Parse(scpi_t * context, char * data, int len) {
    scpi_bool_t result = TRUE;
    scpi_parser_state_t * state;
    int r;
    scpi_token_t cmd_prev = {SCPI_TOKEN_UNKNOWN, NULL, 0};

    if (context == NULL) {
        return FALSE;
    }

    state = &context->parser_state;
    context->output_count = 0;

    while (1) {
        r = scpiParser_detectProgramMessageUnit(state, data, len);

        if (state->programHeader.type == SCPI_TOKEN_INVALID) {
            SCPI_ErrorPush(context, SCPI_ERROR_INVALID_CHARACTER);
            result = FALSE;
        } else if (state->programHeader.len > 0) {

            composeCompoundCommand(&cmd_prev, &state->programHeader);

            if (findCommandHeader(context, state->programHeader.ptr, state->programHeader.len)) {

                context->param_list.lex_state.buffer = state->programData.ptr;
                context->param_list.lex_state.pos = context->param_list.lex_state.buffer;
                context->param_list.lex_state.len = state->programData.len;
                context->param_list.cmd_raw.data = state->programHeader.ptr;
                context->param_list.cmd_raw.position = 0;
                context->param_list.cmd_raw.length = state->programHeader.len;

                result &= processCommand(context);
                cmd_prev = state->programHeader;
            } else {
                SCPI_ErrorPush(context, SCPI_ERROR_UNDEFINED_HEADER);
                result = FALSE;
            }
        }

        if (r < len) {
            data += r;
            len -= r;
        } else {
            break;
        }

    }

    /* conditionaly write new line */
    writeNewLine(context);

    return result;
}

/**
 * Initialize SCPI context structure
 * @param context
 * @param command_list
 * @param buffer
 * @param interface
 */
void SCPI_Init(scpi_t * context) {
    if (context->idn[0] == NULL) {
        context->idn[0] = SCPI_DEFAULT_1_MANUFACTURE;
    }
    if (context->idn[1] == NULL) {
        context->idn[1] = SCPI_DEFAULT_2_MODEL;
    }
    if (context->idn[2] == NULL) {
        context->idn[2] = SCPI_DEFAULT_3;
    }
    if (context->idn[3] == NULL) {
        context->idn[3] = SCPI_DEFAULT_4_REVISION;
    }

    context->buffer.position = 0;
    SCPI_ErrorInit(context);
}

/**
 * Interface to the application. Adds data to system buffer and try to search
 * command line termination. If the termination is found or if len=0, command
 * parser is called.
 *
 * @param context
 * @param data - data to process
 * @param len - length of data
 * @return
 */
scpi_bool_t SCPI_Input(scpi_t * context, const char * data, int len) {
    scpi_bool_t result = TRUE;
    size_t totcmdlen = 0;
    int cmdlen = 0;

    if (len == 0) {
        context->buffer.data[context->buffer.position] = 0;
        result = SCPI_Parse(context, context->buffer.data, context->buffer.position);
        context->buffer.position = 0;
    } else {
        int buffer_free;

        buffer_free = context->buffer.length - context->buffer.position;
        if (len > (buffer_free - 1)) {
            /* Input buffer overrun - invalidate buffer */
            context->buffer.position = 0;
            context->buffer.data[context->buffer.position] = 0;
            SCPI_ErrorPush(context, SCPI_ERROR_INPUT_BUFFER_OVERRUN);
            return FALSE;
        }
        memcpy(&context->buffer.data[context->buffer.position], data, len);
        context->buffer.position += len;
        context->buffer.data[context->buffer.position] = 0;


        while (1) {
            cmdlen = scpiParser_detectProgramMessageUnit(&context->parser_state, context->buffer.data + totcmdlen, context->buffer.position - totcmdlen);
            totcmdlen += cmdlen;

            if (context->parser_state.termination == SCPI_MESSAGE_TERMINATION_NL) {
                result = SCPI_Parse(context, context->buffer.data, totcmdlen);
                memmove(context->buffer.data, context->buffer.data + totcmdlen, context->buffer.position - totcmdlen);
                context->buffer.position -= totcmdlen;
                totcmdlen = 0;
            } else {
                if (context->parser_state.programHeader.type == SCPI_TOKEN_UNKNOWN) break;
                if (totcmdlen >= context->buffer.position) break;
            }
        }
    }

    return result;
}

/* writing results */

/**
 * Write raw string result to the output
 * @param context
 * @param data
 * @return
 */
size_t SCPI_ResultCharacters(scpi_t * context, const char * data, size_t len) {
    size_t result = 0;
    result += writeDelimiter(context);
    result += writeData(context, data, len);
    context->output_count++;
    return result;
}

/**
 * Return prefix of nondecimal base
 * @param base
 * @return
 */
static const char * getBasePrefix(int8_t base) {
    switch (base) {
        case 2: return "#B";
        case 8: return "#Q";
        case 16: return "#H";
        default: return NULL;
    }
}

/**
 * Write signed/unsigned 32 bit integer value in specific base to the result
 * @param context
 * @param val
 * @param base
 * @param sign
 * @return
 */
static size_t resultUInt32BaseSign(scpi_t * context, uint32_t val, int8_t base, scpi_bool_t sign) {
    char buffer[32 + 1];
    const char * basePrefix;
    size_t result = 0;
    size_t len;

    len = UInt32ToStrBaseSign(val, buffer, sizeof (buffer), base, sign);
    basePrefix = getBasePrefix(base);

    result += writeDelimiter(context);
    if (basePrefix != NULL) {
        result += writeData(context, basePrefix, 2);
    }
    result += writeData(context, buffer, len);
    context->output_count++;
    return result;
}

/**
 * Write signed/unsigned 64 bit integer value in specific base to the result
 * @param context
 * @param val
 * @param base
 * @param sign
 * @return
 */
static size_t resultUInt64BaseSign(scpi_t * context, uint64_t val, int8_t base, scpi_bool_t sign) {
    char buffer[64 + 1];
    const char * basePrefix;
    size_t result = 0;
    size_t len;

    len = UInt64ToStrBaseSign(val, buffer, sizeof (buffer), base, sign);
    basePrefix = getBasePrefix(base);

    result += writeDelimiter(context);
    if (basePrefix != NULL) {
        result += writeData(context, basePrefix, 2);
    }
    result += writeData(context, buffer, len);
    context->output_count++;
    return result;
}

/**
 * Write signed 32 bit integer value to the result
 * @param context
 * @param val
 * @return
 */
size_t SCPI_ResultInt32(scpi_t * context, int32_t val) {
    return resultUInt32BaseSign(context, val, 10, TRUE);
}

/**
 * Write unsigned 32 bit integer value in specific base to the result
 * Write signed/unsigned 32 bit integer value in specific base to the result
 * @param context
 * @param val
 * @return
 */
size_t SCPI_ResultUInt32Base(scpi_t * context, uint32_t val, int8_t base) {
    return resultUInt32BaseSign(context, val, base, FALSE);
}

/**
 * Write signed 64 bit integer value to the result
 * @param context
 * @param val
 * @return
 */
size_t SCPI_ResultInt64(scpi_t * context, int64_t val) {
    return resultUInt64BaseSign(context, val, 10, TRUE);
}

/**
 * Write unsigned 64 bit integer value in specific base to the result
 * @param context
 * @param val
 * @return
 */
size_t SCPI_ResultUInt64Base(scpi_t * context, uint64_t val, int8_t base) {
    return resultUInt64BaseSign(context, val, base, FALSE);
}

/**
 * Write float (32 bit) value to the result
 * @param context
 * @param val
 * @return
 */
size_t SCPI_ResultFloat(scpi_t * context, float val) {
    char buffer[32];
    size_t result = 0;
    size_t len = SCPI_FloatToStr(val, buffer, sizeof (buffer));
    result += writeDelimiter(context);
    result += writeData(context, buffer, len);
    context->output_count++;
    return result;
}

/**
 * Write double (64bit) value to the result
 * @param context
 * @param val
 * @return
 */
size_t SCPI_ResultDouble(scpi_t * context, double val) {
    char buffer[32];
    size_t result = 0;
    size_t len = SCPI_DoubleToStr(val, buffer, sizeof (buffer));
    result += writeDelimiter(context);
    result += writeData(context, buffer, len);
    context->output_count++;
    return result;
}

/**
 * Write string withn " to the result
 * @param context
 * @param data
 * @return
 */
size_t SCPI_ResultText(scpi_t * context, const char * data) {
    size_t result = 0;
    result += writeDelimiter(context);
    result += writeData(context, "\"", 1);
    // TODO: convert " to ""
    result += writeData(context, data, strlen(data));
    result += writeData(context, "\"", 1);
    context->output_count++;
    return result;
}

static size_t resultBufferInt16Bin(scpi_t * context, const int16_t *data, size_t size) {
    size_t result = 0;

    result += writeBinHeader(context, size, sizeof(int16_t));

    if (result == 0) {
        return result;
    }

    size_t i;
    for (i = 0; i < size; i++) {
        uint16_t value = htons((uint16_t) data[i]);
        result += writeData(context, (char*)(&value), sizeof(int16_t));
    }
    context->output_binary_count++;
    return result;
}

#include <inttypes.h>
static size_t resultBufferInt16Ascii(scpi_t * context, const int16_t *data, size_t size) {
    size_t result = 0;
    result += writeDelimiter(context);
    result += writeData(context, "{", 1);

    size_t i;
    size_t len;
    char buffer[12];
    for (i = 0; i < size; i++) {
        snprintf(buffer, sizeof (buffer), "%"PRIi16, data[i]);
        len = strlen(buffer);
        // TODO: there were casting issues with the following code
        //len = SCPI_Int32ToStr((int32_t) data[i], buffer, sizeof (buffer));
        result += writeData(context, buffer, len);
        if (i < size-1)
            result += writeData(context, ",", 1);
    }
    result += writeData(context, "}", 1);
    context->output_count++;
    return result;
}

size_t SCPI_ResultBufferInt16(scpi_t * context, const int16_t *data, size_t size) {

    if (context->binary_output == true) {
        return resultBufferInt16Bin(context, data, size);
    }
    else {
        return resultBufferInt16Ascii(context, data, size);
    }
}

static size_t resultBufferFloatBin(scpi_t * context, const float *data, size_t size) {
    size_t result = 0;

    result += writeBinHeader(context, size, sizeof(float));

    if (result == 0) {
        return result;
    }

    size_t i;
    for (i = 0; i < size; i++) {
        float value = hton_f(data[i]);
        result += writeData(context, (char*)(&value), sizeof(float));
    }
    context->output_binary_count++;
    return result;
}

static size_t resultBufferFloatAscii(scpi_t * context, const float *data, size_t size) {
    size_t result = 0;
    result += writeDelimiter(context);
    result += writeData(context, "{", 1);

    size_t i;
    size_t len;
    char buffer[50];
    for (i = 0; i < size; i++) {
        len = SCPI_DoubleToStr(data[i], buffer, sizeof (buffer));
        result += writeData(context, buffer, len);
        if (i < size-1)
            result += writeData(context, ",", 1);
    }
    result += writeData(context, "}", 1);
    context->output_count++;
    return result;
}

size_t SCPI_ResultBufferFloat(scpi_t * context, const float *data, uint32_t size) {

    if (context->binary_output == true) {
        return resultBufferFloatBin(context, data, size);
    }
    else {
        return resultBufferFloatAscii(context, data, size);
    }
}


/* parsing parameters */

/**
 * Write arbitrary block program data to the result
 * @param context
 * @param data
 * @param len
 * @return
 */
size_t SCPI_ResultArbitraryBlock(scpi_t * context, const char * data, size_t len) {
    size_t result = 0;
    char block_header[12];
    size_t header_len;
    block_header[0] = '#';
    SCPI_UInt32ToStrBase((uint32_t) len, block_header + 2, 10, 10);

    header_len = strlen(block_header + 2);
    block_header[1] = (char) (header_len + '0');

    result += writeData(context, block_header, header_len + 2);
    result += writeData(context, data, len);

    context->output_count++;
    return result;
}

/**
 * Write boolean value to the result
 * @param context
 * @param val
 * @return
 */
size_t SCPI_ResultBool(scpi_t * context, scpi_bool_t val) {
    return resultUInt32BaseSign(context, val ? 1 : 0, 10, FALSE);
}

/* parsing parameters */

/**
 * Invalidate token
 * @param token
 * @param ptr
 */
static void invalidateToken(scpi_token_t * token, char * ptr) {
    token->len = 0;
    token->ptr = ptr;
    token->type = SCPI_TOKEN_UNKNOWN;
}

/**
 * Get one parameter from command line
 * @param context
 * @param parameter
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_Parameter(scpi_t * context, scpi_parameter_t * parameter, scpi_bool_t mandatory) {
    lex_state_t * state;

    if (!parameter) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    invalidateToken(parameter, NULL);

    state = &context->param_list.lex_state;

    if (state->pos >= (state->buffer + state->len)) {
        if (mandatory) {
            SCPI_ErrorPush(context, SCPI_ERROR_MISSING_PARAMETER);
        } else {
            parameter->type = SCPI_TOKEN_PROGRAM_MNEMONIC; // TODO: select something different
        }
        return FALSE;
    }
    if (context->input_count != 0) {
        scpiLex_Comma(state, parameter);
        if (parameter->type != SCPI_TOKEN_COMMA) {
            invalidateToken(parameter, NULL);
            SCPI_ErrorPush(context, SCPI_ERROR_INVALID_SEPARATOR);
            return FALSE;
        }
    }

    context->input_count++;

    scpiParser_parseProgramData(&context->param_list.lex_state, parameter);

    switch (parameter->type) {
        case SCPI_TOKEN_HEXNUM:
        case SCPI_TOKEN_OCTNUM:
        case SCPI_TOKEN_BINNUM:
        case SCPI_TOKEN_PROGRAM_MNEMONIC:
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA:
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA_WITH_SUFFIX:
        case SCPI_TOKEN_ARBITRARY_BLOCK_PROGRAM_DATA:
        case SCPI_TOKEN_SINGLE_QUOTE_PROGRAM_DATA:
        case SCPI_TOKEN_DOUBLE_QUOTE_PROGRAM_DATA:
        case SCPI_TOKEN_PROGRAM_EXPRESSION:
            return TRUE;
        default:
            invalidateToken(parameter, NULL);
            SCPI_ErrorPush(context, SCPI_ERROR_INVALID_STRING_DATA);
            return FALSE;
    }
}

/**
 * Detect if parameter is number
 * @param parameter
 * @param suffixAllowed
 * @return
 */
scpi_bool_t SCPI_ParamIsNumber(scpi_parameter_t * parameter, scpi_bool_t suffixAllowed) {
    switch (parameter->type) {
        case SCPI_TOKEN_HEXNUM:
        case SCPI_TOKEN_OCTNUM:
        case SCPI_TOKEN_BINNUM:
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA:
            return TRUE;
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA_WITH_SUFFIX:
            return suffixAllowed;
        default:
            return FALSE;
    }
}

/**
 * Convert parameter to signed/unsigned 32 bit integer
 * @param context
 * @param parameter
 * @param value result
 * @param sign
 * @return TRUE if succesful
 */
static scpi_bool_t ParamSignToUInt32(scpi_t * context, scpi_parameter_t * parameter, uint32_t * value, scpi_bool_t sign) {

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    switch (parameter->type) {
        case SCPI_TOKEN_HEXNUM:
            return strBaseToUInt32(parameter->ptr, value, 16) > 0 ? TRUE : FALSE;
        case SCPI_TOKEN_OCTNUM:
            return strBaseToUInt32(parameter->ptr, value, 8) > 0 ? TRUE : FALSE;
        case SCPI_TOKEN_BINNUM:
            return strBaseToUInt32(parameter->ptr, value, 2) > 0 ? TRUE : FALSE;
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA:
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA_WITH_SUFFIX:
            if (sign) {
                return strBaseToInt32(parameter->ptr, (int32_t *) value, 10) > 0 ? TRUE : FALSE;
            } else {
                return strBaseToUInt32(parameter->ptr, value, 10) > 0 ? TRUE : FALSE;
            }
        default:
            return FALSE;
    }
}

/**
 * Convert parameter to signed/unsigned 64 bit integer
 * @param context
 * @param parameter
 * @param value result
 * @param sign
 * @return TRUE if succesful
 */
static scpi_bool_t ParamSignToUInt64(scpi_t * context, scpi_parameter_t * parameter, uint64_t * value, scpi_bool_t sign) {

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    switch (parameter->type) {
        case SCPI_TOKEN_HEXNUM:
            return strBaseToUInt64(parameter->ptr, value, 16) > 0 ? TRUE : FALSE;
        case SCPI_TOKEN_OCTNUM:
            return strBaseToUInt64(parameter->ptr, value, 8) > 0 ? TRUE : FALSE;
        case SCPI_TOKEN_BINNUM:
            return strBaseToUInt64(parameter->ptr, value, 2) > 0 ? TRUE : FALSE;
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA:
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA_WITH_SUFFIX:
            if (sign) {
                return strBaseToInt64(parameter->ptr, (int64_t *) value, 10) > 0 ? TRUE : FALSE;
            } else {
                return strBaseToUInt64(parameter->ptr, value, 10) > 0 ? TRUE : FALSE;
            }
        default:
            return FALSE;
    }
}

/**
 * Convert parameter to signed 32 bit integer
 * @param context
 * @param parameter
 * @param value result
 * @return TRUE if succesful
 */
scpi_bool_t SCPI_ParamToInt32(scpi_t * context, scpi_parameter_t * parameter, int32_t * value) {
    return ParamSignToUInt32(context, parameter, (uint32_t *) value, TRUE);
}

/**
 * Convert parameter to unsigned 32 bit integer
 * @param context
 * @param parameter
 * @param value result
 * @return TRUE if succesful
 */
scpi_bool_t SCPI_ParamToUInt32(scpi_t * context, scpi_parameter_t * parameter, uint32_t * value) {
    return ParamSignToUInt32(context, parameter, value, FALSE);
}

/**
 * Convert parameter to signed 64 bit integer
 * @param context
 * @param parameter
 * @param value result
 * @return TRUE if succesful
 */
scpi_bool_t SCPI_ParamToInt64(scpi_t * context, scpi_parameter_t * parameter, int64_t * value) {
    return ParamSignToUInt64(context, parameter, (uint64_t *) value, TRUE);
}

/**
 * Convert parameter to unsigned 32 bit integer
 * @param context
 * @param parameter
 * @param value result
 * @return TRUE if succesful
 */
scpi_bool_t SCPI_ParamToUInt64(scpi_t * context, scpi_parameter_t * parameter, uint64_t * value) {
    return ParamSignToUInt64(context, parameter, value, FALSE);
}

/**
 * Convert parameter to float (32 bit)
 * @param context
 * @param parameter
 * @param value result
 * @return TRUE if succesful
 */
scpi_bool_t SCPI_ParamToFloat(scpi_t * context, scpi_parameter_t * parameter, float * value) {
    scpi_bool_t result;
    uint32_t valint;

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    switch (parameter->type) {
        case SCPI_TOKEN_HEXNUM:
        case SCPI_TOKEN_OCTNUM:
        case SCPI_TOKEN_BINNUM:
            result = SCPI_ParamToUInt32(context, parameter, &valint);
            *value = valint;
            break;
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA:
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA_WITH_SUFFIX:
            result = strToFloat(parameter->ptr, value) > 0 ? TRUE : FALSE;
            break;
        default:
            result = FALSE;
    }
    return result;
}

/**
 * Convert parameter to double (64 bit)
 * @param context
 * @param parameter
 * @param value result
 * @return TRUE if succesful
 */
scpi_bool_t SCPI_ParamToDouble(scpi_t * context, scpi_parameter_t * parameter, double * value) {
    scpi_bool_t result;
    uint64_t valint;

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    switch (parameter->type) {
        case SCPI_TOKEN_HEXNUM:
        case SCPI_TOKEN_OCTNUM:
        case SCPI_TOKEN_BINNUM:
            result = SCPI_ParamToUInt64(context, parameter, &valint);
            *value = valint;
            break;
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA:
        case SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA_WITH_SUFFIX:
            result = strToDouble(parameter->ptr, value) > 0 ? TRUE : FALSE;
            break;
        default:
            result = FALSE;
    }
    return result;
}

/**
 * Read floating point float (32 bit) parameter
 * @param context
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamFloat(scpi_t * context, float * value, scpi_bool_t mandatory) {
    scpi_bool_t result;
    scpi_parameter_t param;

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {
        if (SCPI_ParamIsNumber(&param, FALSE)) {
            SCPI_ParamToFloat(context, &param, value);
        } else if (SCPI_ParamIsNumber(&param, TRUE)) {
            SCPI_ErrorPush(context, SCPI_ERROR_SUFFIX_NOT_ALLOWED);
            result = FALSE;
        } else {
            SCPI_ErrorPush(context, SCPI_ERROR_DATA_TYPE_ERROR);
            result = FALSE;
        }
    }
    return result;
}

/**
 * Read floating point double (64 bit) parameter
 * @param context
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamDouble(scpi_t * context, double * value, scpi_bool_t mandatory) {
    scpi_bool_t result;
    scpi_parameter_t param;

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {
        if (SCPI_ParamIsNumber(&param, FALSE)) {
            SCPI_ParamToDouble(context, &param, value);
        } else if (SCPI_ParamIsNumber(&param, TRUE)) {
            SCPI_ErrorPush(context, SCPI_ERROR_SUFFIX_NOT_ALLOWED);
            result = FALSE;
        } else {
            SCPI_ErrorPush(context, SCPI_ERROR_DATA_TYPE_ERROR);
            result = FALSE;
        }
    }
    return result;
}

/**
 * Read signed/unsigned 32 bit integer parameter
 * @param context
 * @param value
 * @param mandatory
 * @param sign
 * @return
 */
static scpi_bool_t ParamSignUInt32(scpi_t * context, uint32_t * value, scpi_bool_t mandatory, scpi_bool_t sign) {
    scpi_bool_t result;
    scpi_parameter_t param;

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {
        if (SCPI_ParamIsNumber(&param, FALSE)) {
            result = ParamSignToUInt32(context, &param, value, sign);
        } else if (SCPI_ParamIsNumber(&param, TRUE)) {
            SCPI_ErrorPush(context, SCPI_ERROR_SUFFIX_NOT_ALLOWED);
            result = FALSE;
        } else {
            SCPI_ErrorPush(context, SCPI_ERROR_DATA_TYPE_ERROR);
            result = FALSE;
        }
    }
    return result;
}

/**
 * Read signed/unsigned 64 bit integer parameter
 * @param context
 * @param value
 * @param mandatory
 * @param sign
 * @return
 */
static scpi_bool_t ParamSignUInt64(scpi_t * context, uint64_t * value, scpi_bool_t mandatory, scpi_bool_t sign) {
    scpi_bool_t result;
    scpi_parameter_t param;

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {
        if (SCPI_ParamIsNumber(&param, FALSE)) {
            result = ParamSignToUInt64(context, &param, value, sign);
        } else if (SCPI_ParamIsNumber(&param, TRUE)) {
            SCPI_ErrorPush(context, SCPI_ERROR_SUFFIX_NOT_ALLOWED);
            result = FALSE;
        } else {
            SCPI_ErrorPush(context, SCPI_ERROR_DATA_TYPE_ERROR);
            result = FALSE;
        }
    }
    return result;
}

/**
 * Read signed 32 bit integer parameter
 * @param context
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamInt32(scpi_t * context, int32_t * value, scpi_bool_t mandatory) {
    return ParamSignUInt32(context, (uint32_t *) value, mandatory, TRUE);
}

/**
 * Read unsigned 32 bit integer parameter
 * @param context
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamUInt32(scpi_t * context, uint32_t * value, scpi_bool_t mandatory) {
    return ParamSignUInt32(context, value, mandatory, FALSE);
}

/**
 * Read signed 64 bit integer parameter
 * @param context
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamInt64(scpi_t * context, int64_t * value, scpi_bool_t mandatory) {
    return ParamSignUInt64(context, (uint64_t *) value, mandatory, TRUE);
}

/**
 * Read unsigned 64 bit integer parameter
 * @param context
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamUInt64(scpi_t * context, uint64_t * value, scpi_bool_t mandatory) {
    return ParamSignUInt64(context, value, mandatory, FALSE);
}

/**
 * Read character parameter
 * @param context
 * @param value
 * @param len
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamCharacters(scpi_t * context, const char ** value, size_t * len, scpi_bool_t mandatory) {
    scpi_bool_t result;
    scpi_parameter_t param;

    if (!value || !len) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {
        switch (param.type) {
            case SCPI_TOKEN_SINGLE_QUOTE_PROGRAM_DATA:
            case SCPI_TOKEN_DOUBLE_QUOTE_PROGRAM_DATA:
                *value = param.ptr + 1;
                *len = param.len - 2;
                break;
            default:
                *value = param.ptr;
                *len = param.len;
                break;
        }

        // TODO: return also parameter type (ProgramMnemonic, ArbitraryBlockProgramData, SingleQuoteProgramData, DoubleQuoteProgramData
    }

    return result;
}

/**
 * Get arbitrary block program data and returns pointer to data
 * @param context
 * @param value result pointer to data
 * @param len result length of data
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamArbitraryBlock(scpi_t * context, const char ** value, size_t * len, scpi_bool_t mandatory) {
    scpi_bool_t result;
    scpi_parameter_t param;

    if (!value || !len) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {
        if (param.type == SCPI_TOKEN_ARBITRARY_BLOCK_PROGRAM_DATA) {
            *value = param.ptr;
            *len = param.len;
        } else {
            SCPI_ErrorPush(context, SCPI_ERROR_DATA_TYPE_ERROR);
            result = FALSE;
        }
    }

    return result;
}

scpi_bool_t SCPI_ParamCopyText(scpi_t * context, char * buffer, size_t buffer_len, size_t * copy_len, scpi_bool_t mandatory) {
    scpi_bool_t result;
    scpi_parameter_t param;
    size_t i_from;
    size_t i_to;
    char quote;

    if (!buffer || !copy_len) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {

        switch (param.type) {
            case SCPI_TOKEN_SINGLE_QUOTE_PROGRAM_DATA:
            case SCPI_TOKEN_DOUBLE_QUOTE_PROGRAM_DATA:
                quote = param.type == SCPI_TOKEN_SINGLE_QUOTE_PROGRAM_DATA ? '\'' : '"';
                for (i_from = 1, i_to = 0; i_from < (size_t) (param.len - 1); i_from++) {
                    if (i_from >= buffer_len) {
                        break;
                    }
                    buffer[i_to] = param.ptr[i_from];
                    i_to++;
                    if (param.ptr[i_from] == quote) {
                        i_from++;
                    }
                }
                *copy_len = i_to;
                if (i_to < buffer_len) {
                    buffer[i_to] = 0;
                }
                break;
            default:
                SCPI_ErrorPush(context, SCPI_ERROR_DATA_TYPE_ERROR);
                result = FALSE;
        }
    }

    return result;
}

/**
 * Convert parameter to choice
 * @param context
 * @param parameter - should be PROGRAM_MNEMONIC
 * @param options - NULL terminated list of choices
 * @param value - index to options
 * @return
 */
scpi_bool_t SCPI_ParamToChoice(scpi_t * context, scpi_parameter_t * parameter, const scpi_choice_def_t * options, int32_t * value) {
    size_t res;
    scpi_bool_t result = FALSE;

    if (!options || !value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    if (parameter->type == SCPI_TOKEN_PROGRAM_MNEMONIC) {
        for (res = 0; options[res].name; ++res) {
            if (matchPattern(options[res].name, strlen(options[res].name), parameter->ptr, parameter->len, NULL)) {
                *value = options[res].tag;
                result = TRUE;
                break;
            }
        }

        if (!result) {
            SCPI_ErrorPush(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        }
    } else {
        SCPI_ErrorPush(context, SCPI_ERROR_DATA_TYPE_ERROR);
    }

    return result;
}

/**
 * Find tag in choices and returns its first textual representation
 * @param options specifications of choices numbers (patterns)
 * @param tag numerical representatio of choice
 * @param text result text
 * @return TRUE if succesfule, else FALSE
 */
scpi_bool_t SCPI_ChoiceToName(const scpi_choice_def_t * options, int32_t tag, const char ** text) {
    int i;

    for (i = 0; options[i].name != NULL; i++) {
        if (options[i].tag == tag) {
            *text = options[i].name;
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * Read BOOL parameter (0,1,ON,OFF)
 * @param context
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamBool(scpi_t * context, scpi_bool_t * value, scpi_bool_t mandatory) {
    scpi_bool_t result;
    scpi_parameter_t param;
    int32_t intval;

    scpi_choice_def_t bool_options[] = {
        {"OFF", 0},
        {"ON", 1},
        SCPI_CHOICE_LIST_END /* termination of option list */
    };

    if (!value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);

    if (result) {
        if (param.type == SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA) {
            SCPI_ParamToInt32(context, &param, &intval);
            *value = intval ? TRUE : FALSE;
        } else {
            result = SCPI_ParamToChoice(context, &param, bool_options, &intval);
            if (result) {
                *value = intval ? TRUE : FALSE;
            }
        }
    }

    return result;
}

/**
 * Read value from list of options
 * @param context
 * @param options
 * @param value
 * @param mandatory
 * @return
 */
scpi_bool_t SCPI_ParamChoice(scpi_t * context, const scpi_choice_def_t * options, int32_t * value, scpi_bool_t mandatory) {
    scpi_bool_t result;
    scpi_parameter_t param;

    if (!options || !value) {
        SCPI_ErrorPush(context, SCPI_ERROR_SYSTEM_ERROR);
        return FALSE;
    }

    result = SCPI_Parameter(context, &param, mandatory);
    if (result) {
        result = SCPI_ParamToChoice(context, &param, options, value);
    }

    return result;
}

/**
 * Red Pitaya added function
 * TODO, replace with upstream equivalent
 */
scpi_bool_t SCPI_ParamBufferFloat(scpi_t * context, float *data, uint32_t *size, scpi_bool_t mandatory) {
    *size = 0;
    double value;
    while (true) {
        if (!SCPI_ParamDouble(context, &value, mandatory)) {
            break;
        }
        data[*size] = (float) value;
        *size = *size + 1;
        mandatory = false;          // only first is mandatory
    }
    return true;
}

/**
 * Red Pitaya added function
 * TODO, replace with upstream equivalent
 */
scpi_bool_t SCPI_ParamBufferInt32(scpi_t * context, int32_t *data, uint32_t *size, scpi_bool_t mandatory) {
    *size = 0;
    int32_t value;
    while (true) {
        if (!SCPI_ParamInt32(context, &value, mandatory)) {
            break;
        }
        data[*size] = (int32_t) value;
        *size = *size + 1;
        mandatory = false;          // only first is mandatory
    }
    return true;
}

/**
 * Parse one parameter and detect type
 * @param state
 * @param token
 * @return
 */
int scpiParser_parseProgramData(lex_state_t * state, scpi_token_t * token) {
    scpi_token_t tmp;
    int result = 0;
    int wsLen;
    int suffixLen;
    int realLen = 0;
    realLen += scpiLex_WhiteSpace(state, &tmp);

    if (result == 0) result = scpiLex_NondecimalNumericData(state, token);
    if (result == 0) result = scpiLex_CharacterProgramData(state, token);
    if (result == 0) {
        result = scpiLex_DecimalNumericProgramData(state, token);
        if (result != 0) {
            wsLen = scpiLex_WhiteSpace(state, &tmp);
            suffixLen = scpiLex_SuffixProgramData(state, &tmp);
            if (suffixLen > 0) {
                token->len += wsLen + suffixLen;
                token->type = SCPI_TOKEN_DECIMAL_NUMERIC_PROGRAM_DATA_WITH_SUFFIX;
                result = token->len;
            }
        }
    }

    if (result == 0) result = scpiLex_StringProgramData(state, token);
    if (result == 0) result = scpiLex_ArbitraryBlockProgramData(state, token);
    if (result == 0) result = scpiLex_ProgramExpression(state, token);

    realLen += scpiLex_WhiteSpace(state, &tmp);

    return result + realLen;
}

/**
 * Skip all parameters to correctly detect end of command line.
 * @param state
 * @param token
 * @param numberOfParameters
 * @return
 */
int scpiParser_parseAllProgramData(lex_state_t * state, scpi_token_t * token, int * numberOfParameters) {

    int result;
    scpi_token_t tmp;
    int paramCount = 0;

    token->len = -1;
    token->type = SCPI_TOKEN_ALL_PROGRAM_DATA;
    token->ptr = state->pos;


    for (result = 1; result != 0; result = scpiLex_Comma(state, &tmp)) {
        token->len += result;

        if (result == 0) {
            token->type = SCPI_TOKEN_UNKNOWN;
            token->len = 0;
            paramCount = -1;
            break;
        }

        result = scpiParser_parseProgramData(state, &tmp);
        if (tmp.type != SCPI_TOKEN_UNKNOWN) {
            token->len += result;
        } else {
            token->type = SCPI_TOKEN_UNKNOWN;
            token->len = 0;
            paramCount = -1;
            break;
        }
        paramCount++;
    }

    if (token->len == -1) {
        token->len = 0;
    }

    if (numberOfParameters != NULL) {
        *numberOfParameters = paramCount;
    }
    return token->len;
}

/**
 * Skip complete command line - program header and parameters
 * @param state
 * @param buffer
 * @param len
 * @return
 */
int scpiParser_detectProgramMessageUnit(scpi_parser_state_t * state, char * buffer, int len) {
    lex_state_t lex_state;
    scpi_token_t tmp;
    int result = 0;

    lex_state.buffer = lex_state.pos = buffer;
    lex_state.len = len;
    state->numberOfParameters = 0;

    /* ignore whitespace at the begginig */
    scpiLex_WhiteSpace(&lex_state, &tmp);

    if (scpiLex_ProgramHeader(&lex_state, &state->programHeader) >= 0) {
        if (scpiLex_WhiteSpace(&lex_state, &tmp) > 0) {
            scpiParser_parseAllProgramData(&lex_state, &state->programData, &state->numberOfParameters);
        } else {
            invalidateToken(&state->programData, lex_state.pos);
        }
    } else {
        invalidateToken(&state->programHeader, lex_state.buffer);
        invalidateToken(&state->programData, lex_state.buffer);
    }

    if (result == 0) result = scpiLex_NewLine(&lex_state, &tmp);
    if (result == 0) result = scpiLex_Semicolon(&lex_state, &tmp);

    if (!scpiLex_IsEos(&lex_state) && (result == 0)) {
        lex_state.pos++;

        state->programHeader.len = 1;
        state->programHeader.type = SCPI_TOKEN_INVALID;

        invalidateToken(&state->programData, lex_state.buffer);
    }

    if (SCPI_TOKEN_SEMICOLON == tmp.type) {
        state->termination = SCPI_MESSAGE_TERMINATION_SEMICOLON;
    } else if (SCPI_TOKEN_NL == tmp.type) {
        state->termination = SCPI_MESSAGE_TERMINATION_NL;
    } else {
        state->termination = SCPI_MESSAGE_TERMINATION_NONE;
    }

    return lex_state.pos - lex_state.buffer;
}


/**
 * Check current command
 *  - suitable for one handle to multiple commands
 * @param context
 * @param cmd
 * @return
 */
scpi_bool_t SCPI_IsCmd(scpi_t * context, const char * cmd) {
    const char * pattern;

    if (!context->param_list.cmd) {
        return FALSE;
    }

    pattern = context->param_list.cmd->pattern;
    return matchCommand(pattern, cmd, strlen(cmd), NULL, 0, 0);
}

#if USE_COMMAND_TAGS
/**
 * Return the .tag field of the matching scpi_command_t
 * @param context
 * @return
 */
int32_t SCPI_CmdTag(scpi_t * context) {
    if (context->param_list.cmd) {
        return context->param_list.cmd->tag;
    } else {
        return 0;
    }
}
#endif /* USE_COMMAND_TAGS */

scpi_bool_t SCPI_Match(const char * pattern, const char * value, size_t len) {
    return matchCommand(pattern, value, len, NULL, 0, 0);
}

scpi_bool_t SCPI_CommandNumbers(scpi_t * context, int32_t * numbers, size_t len, int32_t default_value) {
    return matchCommand(context->param_list.cmd->pattern, context->param_list.cmd_raw.data, context->param_list.cmd_raw.length, numbers, len, default_value);
}

/**
 * If SCPI_Parameter() returns FALSE, this function can detect, if the parameter
 * is just missing (TRUE) or if there was an error during processing of the command (FALSE)
 * @param parameter
 * @return
 */
scpi_bool_t SCPI_ParamIsValid(scpi_parameter_t * parameter) {
    return parameter->type == SCPI_TOKEN_UNKNOWN ? FALSE : TRUE;
}

/**
 * Returns TRUE if there was an error during parameter handling
 * @param context
 * @return
 */
scpi_bool_t SCPI_ParamErrorOccurred(scpi_t * context) {
    return context->cmd_error;
}
