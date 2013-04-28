/*
 * gcode_parser.cpp - rs274/ngc Gcode parser.
 * Part of TinyG2 project
 *
 * Copyright (c) 2010 - 2013 Alden S. Hart, Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/*
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>					// needed for memcpy, memset
#include <avr/pgmspace.h>			// precursor for xio.h
*/
#include "tinyg2.h"
#include "config.h"
#include "gcode_parser.h"
#include "canonical_machine.h"
#include "util.h"
#include "xio.h"					// for char definitions

#ifdef __cplusplus
extern "C"{
#endif // __cplusplus

struct gcodeParserSingleton {	 	  // struct to manage globals
	uint8_t modals[MODAL_GROUP_COUNT];// collects modal groups in a block
}; struct gcodeParserSingleton gp;

// local helper functions and macros
static stat_t _normalize_gcode_block(char_t *block);
static stat_t _parse_gcode_block(char_t *line);	// Parse the block into structs
static stat_t _execute_gcode_block(void);		// Execute the gcode block
static stat_t _check_gcode_block(void);			// check the block for correctness
static stat_t _get_next_gcode_word(char **pstr, char *letter, float *value);
static stat_t _point(float value);

#define SET_MODAL(m,parm,val) ({gn.parm=val; gf.parm=1; gp.modals[m]+=1; break;})
#define SET_NON_MODAL(parm,val) ({gn.parm=val; gf.parm=1; break;})
#define EXEC_FUNC(f,v) if((uint8_t)gf.v != false) { status = f(gn.v);}

/*
 * gc_gcode_parser() - parse a block (line) of gcode
 *
 *	Top level of gcode parser. Normalizes block and looks for special cases
 */

stat_t gc_gcode_parser(char_t *block)
{
	uint8_t msg_flag = _normalize_gcode_block(block);	// get block ready for parsing
	if (block[0] == NUL) {
		if (msg_flag == true) return (STAT_OK);	// queues messages for display
		return (STAT_NOOP); 
	}
	return(_parse_gcode_block(block));			// parse block & return if error
}

/*
 * _normalize_gcode_block() - normalize a block (line) of gcode in place
 *
 *	Normalization rules
 *	 - convert all letters to upper case
 *	 - remove all white space
 *	 - remove extraneous leading zeros that might be taken to mean Octal during strtof()
 *	 - remove all control and other invalid characters:
 *		chars < 0x20 (control characters)
 *		! $ % ,	; ; ? @ ^ _ ~ " ' <DEL>
 * 
 *	Valid characters in a Gcode block are (see RS274NGC_3 Appendix E)
 *		digits						all digits are passed to interpreter
 *		lower case alpha			all alpha is passed
 *		upper case alpha			all alpha is passed
 *		+ - . / *	< = > 			chars passed to interpreter
 *		| % # ( ) [ ] { } 			chars passed to interpreter
 *		<sp> <tab> 					chars are legal but are not passed
 *		/  							if first, block delete char - omits the block
 *
 *	Comment handling:
 *
 *	 - Comments are not normalized - they are left alone
 *	 - Comments always terminate the block (i.e. embedded comments are not supported)
 *	 - Messages in comments are sent to console 
 *	 - The 'MSG' specifier in comment can have mixed case but cannot cannot have embedded white spaces
 *	 - Normalization returns true if there was a message to display, false otherwise
 *	 - Processing splits string into command and comment portions - cases:
 *		  supported:	COMMAND
 *		  supported:	comment
 *		  supported:	COMMAND comment
 *
 *		unsupported:	COMMAND COMMAND
 *		unsupported:	comment COMMAND
 *		unsupported:	COMMAND comment COMMAND
 *
 *	++++ todo: Support leading and trailing spaces around the MSG specifier
 *	++++ todo: Refactor to reject Octal numbers (leading 0's)
 */

static uint8_t _normalize_gcode_block(char_t *block) 
{
	char_t c;
	char_t *comment=0;	// comment pointer - first char past opening paren
	uint8_t i=0; 		// index for incoming characters
	uint8_t j=0;		// index for normalized characters

	if (block[0] == '/') {					// discard deleted blocks
		block[0] = NUL;
		return (false);
	}
	if (block[0] == '?') {					// trap and return ? command
		return (false);
	}
	// normalize the command block & mark the comment(if any)
	while ((c = toupper(block[i++])) != NUL) {
		if ((isupper(c)) || (isdigit(c))) {	// capture common chars
		 	block[j++] = c; 
			continue;
		}
		if (c == '(') {						// detect & handle comments
			block[j] = NUL;
			comment = &block[i]; 
			break;
		}
		if (c <= ' ') continue;				// toss controls & whitespace
		if (c == DEL) continue;				// toss DELETE (0x7F)
		if (strchr("!$%,;:?@^_~`\'\"", c))	// toss invalid punctuation
			continue;
		block[j++] = c;
	}
	block[j] = NUL;							// terminate the command
	if (comment != 0) {
		if ((toupper(comment[0]) == 'M') && 
			(toupper(comment[1]) == 'S') &&
			(toupper(comment[2]) == 'G')) {
			i=0;
			comment +=3;					// skip past the leading chars
			while ((c = comment[i++]) != NUL) {// remove trailing parenthesis
				if (c == ')') {
					comment[--i] = NUL;
					break;
				}
			}
			(void)cm_message(comment);
			return (true);
		}
	}
	return (false);
}

/*
 * _parse_gcode_block() - parses one line of NULL terminated G-Code. 
 *
 *	All the parser does is load the state values in gn (next model state),
 *	and flags in gf (model state flags). The execute routine applies them.
 *	The line is assumed to contain only uppercase characters and signed 
 *  floats (no whitespace).
 *
 *	A number of implicit things happen when the gn struct is zeroed:
 *	  - inverse feed rate mode is canceled - set back to units_per_minute mode
 */

static stat_t _parse_gcode_block(char_t *buf) 
{
	char *pstr = (char *)buf;		// persistent pointer into gcode block for parsing words
  	char letter;					// parsed letter, eg.g. G or X or Y
	float value;					// value parsed from letter (e.g. 2 for G2)
	stat_t status = STAT_OK;

	// set initial state for new move 
	memset(&gp, 0, sizeof(gp));		// clear all parser values
	memset(&gf, 0, sizeof(gf));		// clear all next-state flags
	memset(&gn, 0, sizeof(gn));		// clear all next-state values
	gn.motion_mode = cm_get_motion_mode();	// get motion mode from previous block

  	// extract commands and parameters
	while((status = _get_next_gcode_word(&pstr, &letter, &value)) == STAT_OK) {
		switch(letter) {
			case 'G':
				switch((uint8_t)value) {
					case 0:  SET_MODAL (MODAL_GROUP_G1, motion_mode, MOTION_MODE_STRAIGHT_TRAVERSE);
					case 1:  SET_MODAL (MODAL_GROUP_G1, motion_mode, MOTION_MODE_STRAIGHT_FEED);
					case 2:  SET_MODAL (MODAL_GROUP_G1, motion_mode, MOTION_MODE_CW_ARC);
					case 3:  SET_MODAL (MODAL_GROUP_G1, motion_mode, MOTION_MODE_CCW_ARC);
					case 4:  SET_NON_MODAL (next_action, NEXT_ACTION_DWELL);
					case 10: SET_MODAL (MODAL_GROUP_G0, next_action, NEXT_ACTION_SET_COORD_DATA);
					case 17: SET_MODAL (MODAL_GROUP_G2, select_plane, CANON_PLANE_XY);
					case 18: SET_MODAL (MODAL_GROUP_G2, select_plane, CANON_PLANE_XZ);
					case 19: SET_MODAL (MODAL_GROUP_G2, select_plane, CANON_PLANE_YZ);
					case 20: SET_MODAL (MODAL_GROUP_G6, units_mode, INCHES);
					case 21: SET_MODAL (MODAL_GROUP_G6, units_mode, MILLIMETERS);
					case 28: {
						switch (_point(value)) {
							case 0: SET_MODAL (MODAL_GROUP_G0, next_action, NEXT_ACTION_GOTO_G28_POSITION);
							case 1: SET_MODAL (MODAL_GROUP_G0, next_action, NEXT_ACTION_SET_G28_POSITION); 
							case 2: SET_NON_MODAL (next_action, NEXT_ACTION_SEARCH_HOME); 
							case 3: SET_NON_MODAL (next_action, NEXT_ACTION_SET_ABSOLUTE_ORIGIN);
							default: status = STAT_UNRECOGNIZED_COMMAND;
						}
						break;
					}
					case 30: {
						switch (_point(value)) {
							case 0: SET_MODAL (MODAL_GROUP_G0, next_action, NEXT_ACTION_GOTO_G30_POSITION);
							case 1: SET_MODAL (MODAL_GROUP_G0, next_action, NEXT_ACTION_SET_G30_POSITION); 
							default: status = STAT_UNRECOGNIZED_COMMAND;
						}
						break;
					}
/*					case 38: 
						switch (_point(value)) {
							case 2: SET_NON_MODAL (next_action, NEXT_ACTION_STRAIGHT_PROBE); 
							default: status = STAT_UNRECOGNIZED_COMMAND;
						}
						break;
					}
*/					case 40: break;	// ignore cancel cutter radius compensation
					case 49: break;	// ignore cancel tool length offset comp.
					case 53: SET_NON_MODAL (absolute_override, true);
					case 54: SET_MODAL (MODAL_GROUP_G12, coord_system, G54);
					case 55: SET_MODAL (MODAL_GROUP_G12, coord_system, G55);
					case 56: SET_MODAL (MODAL_GROUP_G12, coord_system, G56);
					case 57: SET_MODAL (MODAL_GROUP_G12, coord_system, G57);
					case 58: SET_MODAL (MODAL_GROUP_G12, coord_system, G58);
					case 59: SET_MODAL (MODAL_GROUP_G12, coord_system, G59);
					case 61: {
						switch (_point(value)) {
							case 0: SET_MODAL (MODAL_GROUP_G13, path_control, PATH_EXACT_PATH);
							case 1: SET_MODAL (MODAL_GROUP_G13, path_control, PATH_EXACT_STOP); 
							default: status = STAT_UNRECOGNIZED_COMMAND;
						}
						break;
					}
					case 64: SET_MODAL (MODAL_GROUP_G13,path_control, PATH_CONTINUOUS);
					case 80: SET_MODAL (MODAL_GROUP_G1, motion_mode,  MOTION_MODE_CANCEL_MOTION_MODE);
					case 90: SET_MODAL (MODAL_GROUP_G3, distance_mode, ABSOLUTE_MODE);
					case 91: SET_MODAL (MODAL_GROUP_G3, distance_mode, INCREMENTAL_MODE);
					case 92: {
						switch (_point(value)) {
							case 0: SET_MODAL (MODAL_GROUP_G0, next_action, NEXT_ACTION_SET_ORIGIN_OFFSETS);
							case 1: SET_NON_MODAL (next_action, NEXT_ACTION_RESET_ORIGIN_OFFSETS);
							case 2: SET_NON_MODAL (next_action, NEXT_ACTION_SUSPEND_ORIGIN_OFFSETS);
							case 3: SET_NON_MODAL (next_action, NEXT_ACTION_RESUME_ORIGIN_OFFSETS); 
							default: status = STAT_UNRECOGNIZED_COMMAND;
						}
						break;
					}
					case 93: SET_MODAL (MODAL_GROUP_G5, inverse_feed_rate_mode, true);
					case 94: SET_MODAL (MODAL_GROUP_G5, inverse_feed_rate_mode, false);
					default: status = STAT_UNRECOGNIZED_COMMAND;
				}
				break;

			case 'M':
				switch((uint8_t)value) {
					case 0: case 1: 
							SET_MODAL (MODAL_GROUP_M4, program_flow, PROGRAM_STOP);
					case 2: case 30: case 60:
							SET_MODAL (MODAL_GROUP_M4, program_flow, PROGRAM_END);
					case 3: SET_MODAL (MODAL_GROUP_M7, spindle_mode, SPINDLE_CW);
					case 4: SET_MODAL (MODAL_GROUP_M7, spindle_mode, SPINDLE_CCW);
					case 5: SET_MODAL (MODAL_GROUP_M7, spindle_mode, SPINDLE_OFF);
					case 6: SET_NON_MODAL (change_tool, true);
					case 7: SET_MODAL (MODAL_GROUP_M8, mist_coolant, true);
					case 8: SET_MODAL (MODAL_GROUP_M8, flood_coolant, true);
					case 9: SET_MODAL (MODAL_GROUP_M8, flood_coolant, false);
					case 48: SET_MODAL (MODAL_GROUP_M9, override_enables, true);
					case 49: SET_MODAL (MODAL_GROUP_M9, override_enables, false);
					case 50: SET_MODAL (MODAL_GROUP_M9, feed_rate_override_enable, true); // conditionally true
					case 51: SET_MODAL (MODAL_GROUP_M9, spindle_override_enable, true);	  // conditionally true
					default: status = STAT_UNRECOGNIZED_COMMAND;
				}
				break;

			case 'T': SET_NON_MODAL (tool, (uint8_t)trunc(value));
			case 'F': SET_NON_MODAL (feed_rate, value);
			case 'P': SET_NON_MODAL (parameter, value);				// used for dwell time, G10 coord select
			case 'S': SET_NON_MODAL (spindle_speed, value); 
			case 'X': SET_NON_MODAL (target[AXIS_X], value);
			case 'Y': SET_NON_MODAL (target[AXIS_Y], value);
			case 'Z': SET_NON_MODAL (target[AXIS_Z], value);
			case 'A': SET_NON_MODAL (target[AXIS_A], value);
			case 'B': SET_NON_MODAL (target[AXIS_B], value);
			case 'C': SET_NON_MODAL (target[AXIS_C], value);
		//	case 'U': SET_NON_MODAL (target[AXIS_U], value);		// reserved
		//	case 'V': SET_NON_MODAL (target[AXIS_V], value);		// reserved
		//	case 'W': SET_NON_MODAL (target[AXIS_W], value);		// reserved
			case 'I': SET_NON_MODAL (arc_offset[0], value);
			case 'J': SET_NON_MODAL (arc_offset[1], value);
			case 'K': SET_NON_MODAL (arc_offset[2], value);
			case 'R': SET_NON_MODAL (arc_radius, value);
			case 'N': SET_NON_MODAL (linenum,(uint32_t)value);		// line number
			case 'L': break;										// not used for anything
			default: status = STAT_UNRECOGNIZED_COMMAND;
		}
		if(status != STAT_OK) break;
	}
	if ((status != STAT_OK) && (status != STAT_COMPLETE)) return (status);
	ritorno(_check_gcode_block());			// perform Gcode error checking
	return (_execute_gcode_block());		// if successful execute the block
}

/*
 * _execute_gcode_block() - execute parsed block
 *
 *  Conditionally (based on whether a flag is set in gf) call the canonical 
 *	machining functions in order of execution as per RS274NGC_3 table 8 
 *  (below, with modifications):
 *
 *	    0. record the line number
 *		1. comment (includes message) [handled during block normalization]
 *		2. set feed rate mode (G93, G94 - inverse time or per minute)
 *		3. set feed rate (F)
 *		3a. set feed override rate (M50.1)
 *		3a. set traverse override rate (M50.2)
 *		4. set spindle speed (S)
 *		4a. set spindle override rate (M51.1)
 *		5. select tool (T)
 *		6. change tool (M6)
 *		7. spindle on or off (M3, M4, M5)
 *		8. coolant on or off (M7, M8, M9)
 *		9. enable or disable overrides (M48, M49, M50, M51)
 *		10. dwell (G4)
 *		11. set active plane (G17, G18, G19)
 *		12. set length units (G20, G21)
 *		13. cutter radius compensation on or off (G40, G41, G42)
 *		14. cutter length compensation on or off (G43, G49)
 *		15. coordinate system selection (G54, G55, G56, G57, G58, G59)
 *		16. set path control mode (G61, G61.1, G64)
 *		17. set distance mode (G90, G91)
 *		18. set retract mode (G98, G99)
 *		19a. homing functions (G28.2, G28.3, G28.1, G28, G30)
 *		19b. change coordinate system data (G10)
 *		19c. set axis offsets (G92, G92.1, G92.2, G92.3)
 *		20. perform motion (G0 to G3, G80-G89) as modified (possibly) by G53
 *		21. stop and end (M0, M1, M2, M30, M60)
 *
 *	Values in gn are in original units and should not be unit converted prior 
 *	to calling the canonical functions (which do the unit conversions)
 */

static stat_t _execute_gcode_block()
{
	uint8_t status = STAT_OK;

	cm_set_model_linenum(gn.linenum);
	EXEC_FUNC(cm_set_inverse_feed_rate_mode, inverse_feed_rate_mode);
	EXEC_FUNC(cm_set_feed_rate, feed_rate);
	EXEC_FUNC(cm_feed_rate_override_factor, feed_rate_override_factor);
	EXEC_FUNC(cm_traverse_override_factor, traverse_override_factor);
	EXEC_FUNC(cm_set_spindle_speed, spindle_speed);
	EXEC_FUNC(cm_spindle_override_factor, spindle_override_factor);
	EXEC_FUNC(cm_select_tool, tool);
	EXEC_FUNC(cm_change_tool, tool);
	EXEC_FUNC(cm_spindle_control, spindle_mode); 	// spindle on or off
	EXEC_FUNC(cm_mist_coolant_control, mist_coolant); 
	EXEC_FUNC(cm_flood_coolant_control, flood_coolant);	// also disables mist coolant if OFF 
	EXEC_FUNC(cm_feed_rate_override_enable, feed_rate_override_enable);
	EXEC_FUNC(cm_traverse_override_enable, traverse_override_enable);
	EXEC_FUNC(cm_spindle_override_enable, spindle_override_enable);
	EXEC_FUNC(cm_override_enables, override_enables);

	if (gn.next_action == NEXT_ACTION_DWELL) { 		// G4 - dwell
		ritorno(cm_dwell(gn.parameter));			// return if error, otherwise complete the block
	}
	EXEC_FUNC(cm_select_plane, select_plane);
	EXEC_FUNC(cm_set_units_mode, units_mode);
	//--> cutter radius compensation goes here
	//--> cutter length compensation goes here
	EXEC_FUNC(cm_set_coord_system, coord_system);
	EXEC_FUNC(cm_set_path_control, path_control);
	EXEC_FUNC(cm_set_distance_mode, distance_mode);
	//--> set retract mode goes here

	switch (gn.next_action) {
		case NEXT_ACTION_SEARCH_HOME: { status = cm_homing_cycle_start(); break;}								// G28.2
//		case NEXT_ACTION_STRAIGHT_PROBE: { status = cm_probe_cycle_start(); break;}
		case NEXT_ACTION_SET_ABSOLUTE_ORIGIN: { status = cm_set_absolute_origin(gn.target, gf.target); break;}	// G28.3
		case NEXT_ACTION_SET_G28_POSITION: { status = cm_set_g28_position(); break;}							// G28.1
		case NEXT_ACTION_GOTO_G28_POSITION: { status = cm_goto_g28_position(gn.target, gf.target); break;}		// G28
		case NEXT_ACTION_SET_G30_POSITION: { status = cm_set_g30_position(); break;}							// G30.1
		case NEXT_ACTION_GOTO_G30_POSITION: { status = cm_goto_g30_position(gn.target, gf.target); break;}		// G30	

		case NEXT_ACTION_SET_COORD_DATA: { status = cm_set_coord_offsets(gn.parameter, gn.target, gf.target); break;}
		case NEXT_ACTION_SET_ORIGIN_OFFSETS: { status = cm_set_origin_offsets(gn.target, gf.target); break;}
		case NEXT_ACTION_RESET_ORIGIN_OFFSETS: { status = cm_reset_origin_offsets(); break;}
		case NEXT_ACTION_SUSPEND_ORIGIN_OFFSETS: { status = cm_suspend_origin_offsets(); break;}
		case NEXT_ACTION_RESUME_ORIGIN_OFFSETS: { status = cm_resume_origin_offsets(); break;}

		case NEXT_ACTION_DEFAULT: { 
			cm_set_absolute_override(gn.absolute_override);	// apply override setting to gm struct
			switch (gn.motion_mode) {
				case MOTION_MODE_CANCEL_MOTION_MODE: { gm.motion_mode = gn.motion_mode; break;}
				case MOTION_MODE_STRAIGHT_TRAVERSE: { status = cm_straight_traverse(gn.target, gf.target); break;}
				case MOTION_MODE_STRAIGHT_FEED: { status = cm_straight_feed(gn.target, gf.target); break;}
				case MOTION_MODE_CW_ARC: case MOTION_MODE_CCW_ARC:
					// gf.radius sets radius mode if radius was collected in gn
					{ status = cm_arc_feed(gn.target, gf.target, gn.arc_offset[0], gn.arc_offset[1],
								gn.arc_offset[2], gn.arc_radius, gn.motion_mode); break;}
			}
		}
	}
	cm_set_absolute_override(false);		// un-set abs overrride (for reporting purposes) 
	if (gf.program_flow == true) {
		// do the M stops: M0, M1, M2, M30, M60
	}
	return (status);
}

/*
 * _check_gcode_block() - return a STAT_ error if an error is detected
 */

static stat_t _check_gcode_block()
{
	// Check for modal group violations. From NIST, section 3.4 "It is an error 
	// to put a G-code from group 1 and a G-code from group 0 on the same line 
	// if both of them use axis words. If an axis word-using G-code from group 
	// 1 is implicitly in effect on a line (by having been activated on an 
	// earlier line), and a group 0 G-code that uses axis words appears on the 
	// line, the activity of the group 1 G-code is suspended for that line. 
	// The axis word-using G-codes from group 0 are G10, G28, G30, and G92"
//	if ((gp.modals[MODAL_GROUP_G0] == true) && (gp.modals[MODAL_GROUP_G1] == true)) {
//		return (STAT_MODAL_GROUP_VIOLATION);
//	}
	
	// look for commands that require an axis word to be present
//	if ((gp.modals[MODAL_GROUP_G0] == true) || (gp.modals[MODAL_GROUP_G1] == true)) {
//		if (_axis_changed() == false)
//		return (STAT_GCODE_AXIS_WORD_MISSING);
//	}
	return (STAT_OK);
}

/*
 * helpers
 */

/*
 * _get_next_gcode_word() - get gcode word consisting of a letter and a value
 *
 *	This function requires the Gcode string to be normalized.
 *	Normalization must remove any leading zeros or they will be converted to Octal
 */
static stat_t _get_next_gcode_word(char **pstr, char *letter, float *value) 
{
	if (**pstr == NUL) { return (STAT_COMPLETE); }	// no more words

	// get letter part
	if(isupper(**pstr) == false) { return (STAT_EXPECTED_COMMAND_LETTER); }
	*letter = **pstr;
	(*pstr)++;
	
	// X-axis-becomes-a-hexadecimal-number get-value case, e.g. G0X100 --> G255
	if ((**pstr == '0') && (*(*pstr+1) == 'X')) {
		*value = 0;
		(*pstr)++;
		return (STAT_OK);		// pointer points to X
	}

	// get-value general case
	char *end; 
	*value = strtof(*pstr, &end);
	if(end == *pstr) { return(STAT_BAD_NUMBER_FORMAT); }	// more robust test then checking for value=0;
	*pstr = end;
	return (STAT_OK);			// pointer points to next character after the word
}

static uint8_t _point(float value) 
{
	return((uint8_t)(value*10 - trunc(value)*10));	// isolate the decimal point as an int
}

#ifdef __cplusplus
}
#endif

