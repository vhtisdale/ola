/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Action.cpp
 * Copyright (C) 2011 Simon Newton
 */

#include <ola/Logging.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

#include "tools/dmx_trigger/Action.h"
#include "tools/dmx_trigger/VariableInterpolator.h"

using std::string;
using std::vector;


/**
 * Assign the value to the variable.
 */
void VariableAssignmentAction::Execute(Context *context, uint8_t) {
  string interpolated_value;
  bool ok = InterpolateVariables(m_value, &interpolated_value, *context);

  if (ok) {
    if (context)
      context->Update(m_variable, interpolated_value);
  } else {
    OLA_WARN << "Failed to expand variables in " << m_value;
  }
}


/**
 * Execute the command
 */
void CommandAction::Execute(Context *context, uint8_t) {
  pid_t pid;
  char **args = BuildArgList(context);

  if ((pid = fork()) < 0) {
    OLA_FATAL << "Could not fork to exec " << m_command;
    return;
  } else if (pid) {
    // parent
    OLA_DEBUG << "child for " << m_command << " is " << pid;
    FreeArgList(args);
    return;
  }

  execvp(m_command.c_str(), args);
}


/**
 * Interpolate all the arguments, and return a pointer to an array of char*
 * pointers which can be passed to exec()
 */
char **CommandAction::BuildArgList(const Context *context) {
  // we need to add the command here as the first arg, also +1 for the NULL
  unsigned int array_size = m_arguments.size() + 2;
  char **args = new char*[array_size];
  memset(args, 0, sizeof(char*) * array_size);

  args[0] = StringToDynamicChar(m_command);

  vector<string>::const_iterator iter = m_arguments.begin();
  unsigned int i = 1;
  for (; iter != m_arguments.end(); i++, iter++) {
    string result;
    if (!InterpolateVariables(*iter, &result, *context)) {
      FreeArgList(args);
      return NULL;
    }
    args[i] = StringToDynamicChar(result);
  }
  return args;
}


/**
 * Free the arg array.
 */
void CommandAction::FreeArgList(char **args) {
  char **ptr = args;
  while (*ptr)
    delete[] *ptr++;
  delete[] args;
}


/**
 * Build a null terminated char* on the heap from a string.
 * @param str the string object to convert
 * @returns a new[] char with the contents of the string.
 */
char *CommandAction::StringToDynamicChar(const string &str) {
  unsigned int str_size = str.size() + 1;
  char *s = new char[str_size];
  strncpy(s, str.c_str(), str_size);
  return s;
}


/**
 * Return the interval as a string.
 */
string ActionInterval::AsString() const {
  std::stringstream str;
  if (m_lower == m_upper) {
    str << static_cast<int>(m_lower);
  } else {
    str << "[" << static_cast<int>(m_lower) << ", " <<
      static_cast<int>(m_upper) << "]";
  }
  return str.str();
}


/**
 * Stream operator
 */
std::ostream& operator<<(std::ostream &out, const ActionInterval &i) {
  return out << i.AsString();
}


/**
 * Cleanup
 */
SlotActions::~SlotActions() {
  ActionVector::iterator iter = m_actions.begin();
  for (; iter != m_actions.end(); ++iter)
    delete *iter;
  m_actions.clear();

  if (m_default_action)
    m_default_action->DeRef();
}


/**
 * Attempt to associated an Action with a interval
 * @param lower_value the lower bound of the interval
 * @param upper_value the upper bound of the interval
 * @param action the Action to take if the value is contained within this
 *   interval.
 * @returns true if the interval was added, false otherwise.
 */
bool SlotActions::AddAction(uint8_t lower_value,
                            uint8_t upper_value,
                            Action *action) {
  if (lower_value > upper_value) {
    OLA_WARN << "Attempting to add a interval with lower (" <<
      static_cast<int>(lower_value) << ") > upper (" <<
      static_cast<int>(upper_value) << ")";
    return false;
  }

  ActionInterval *action_interval = new ActionInterval(lower_value,
                                                       upper_value,
                                                       action);
  if (m_actions.empty()) {
    m_actions.push_back(action_interval);
    return true;
  }

  ActionVector::iterator lower = m_actions.begin();
  if (IntervalsIntersect(action_interval, *lower)) {
    delete action_interval;
    return false;
  }

  if (*action_interval < **lower) {
    m_actions.insert(lower, action_interval);
    return true;
  }

  ActionVector::iterator upper = m_actions.end();
  upper--;
  if (IntervalsIntersect(action_interval, *upper)) {
    delete action_interval;
    return false;
  }

  if (**upper < *action_interval) {
    // action_interval goes at the end
    m_actions.insert(m_actions.end(), action_interval);
    return true;
  }

  if (lower == upper) {
    OLA_WARN << "Inconsistent interval state, adding " << action_interval <<
        ", to " << IntervalsAsString(m_actions.begin(), m_actions.end());
    delete action_interval;
    return false;
  }

  /**
   * We need to insert the interval between the lower and upper
   * @pre lower != upper
   * @pre the new interval falls between lower and upper
   * @pre lower and upper don't intersect with the new interval
   */
  while (true) {
    if (lower + 1 == upper) {
      // the new interval goes between the two
      m_actions.insert(upper, action_interval);
      return true;
    }

    unsigned int difference = upper - lower;
    ActionVector::iterator mid = lower + difference / 2;

    if (IntervalsIntersect(action_interval, *mid)) {
      delete action_interval;
      return false;
    }

    if (*action_interval < **mid) {
      upper = mid;
    } else if (**mid < *action_interval) {
      lower = mid;
    } else {
      OLA_WARN << "Inconsistent intervals detected when inserting: " <<
        action_interval << ", intervals: " <<
        IntervalsAsString(lower, upper);
      delete action_interval;
      return false;
    }
  }
  return true;
}


/**
 * Set the default action. If a default already exists this replaces it.
 * @param action the action to install as the default
 * @returns true if there was already a default action.
 */
bool SlotActions::SetDefaultAction(Action *action) {
  bool previous_default_set = false;
  action->Ref();

  if (m_default_action) {
    previous_default_set = true;
    action->DeRef();
  }
  m_default_action = action;
  return previous_default_set;
}


/**
 * Lookup the action for a value, and if we find one, execute it. Otherwise
 * execute the default action if there is one.
 * @param value the value to look up.
 */
void SlotActions::TakeAction(Context *context, uint8_t value) {
  // set the context correctly
  if (context) {
    context->SetSlotOffset(m_slot_offset);
    context->SetSlotValue(value);
  }

  ActionInterval *action_interval = LocateMatchingAction(value);
  if (action_interval) {
    Action *action = action_interval->GetAction();
    if (action)
      action->Execute(context, value);
  } else if (m_default_action) {
    m_default_action->Execute(context, value);
  }
}


/**
 * Return the intervals as a string, useful for debugging
 * @returns the intervals as a string.
 */
string SlotActions::IntervalsAsString() const {
  return IntervalsAsString(m_actions.begin(), m_actions.end());
}


/**
 * Given two interval iterators, first and last, return true if the value is
 * contained within the lower and upper bounds of the intervals.
 */
bool SlotActions::ValueWithinIntervals(uint8_t value,
                                       const ActionInterval &lower_interval,
                                       const ActionInterval &upper_interval) {
  return lower_interval.Lower() <= value && value <= upper_interval.Upper();
}


/**
 * Check if two ActionIntervals intersect.
 */
bool SlotActions::IntervalsIntersect(const ActionInterval *a1,
                                     const ActionInterval *a2) {
  if (a1->Intersects(*a2)) {
    OLA_WARN << "Interval " << *a1 << " overlaps " << *a2;
    return true;
  }
  return false;
}


/**
 * Given a value, find the matching ActionInterval.
 * @param value the value to search for
 * @returns the ActionInterval containing the value, or NULL if there isn't
 *   one.
 */
ActionInterval *SlotActions::LocateMatchingAction(uint8_t value) {
  if (m_actions.empty())
    return NULL;

  ActionVector::iterator lower = m_actions.begin();
  ActionVector::iterator upper = m_actions.end();
  upper--;
  if (!ValueWithinIntervals(value, **lower, **upper))
    return NULL;

  // ok, we know the value lies between the intervals we have, first exclude
  // the endpoints
  if ((*lower)->Contains(value))
    return *lower;

  if ((*upper)->Contains(value))
    return *upper;

  // value isn't at the lower or upper interval, but lies somewhere between
  // the two.
  // @pre lower != upper
  // @pre !lower.Contains(value) && !upper.Contains(value)
  while (true) {
    unsigned int difference = upper - lower;
    ActionVector::iterator mid = lower + difference / 2;

    if (mid == lower)
      // lower doesn't contain the value, so we don't have it
      return NULL;

    if ((*mid)->Contains(value)) {
      return *mid;
    } else if (value <= (*mid)->Lower()) {
      upper = mid;
    } else if (value >= (*mid)->Upper()) {
      lower = mid;
    } else {
      OLA_WARN << "Inconsistent intervals detected when looking for: " <<
        static_cast<int>(value) << ", intervals: " <<
        IntervalsAsString(lower, upper);
      return NULL;
    }
  }
}


/**
 * Format the intervals between the two iterators as a string
 * @param start an iterator pointing to the first interval
 * @param end an iterator pointing to the last interval
 * @return a string version of the intervals.
 */
string SlotActions::IntervalsAsString(
    const ActionVector::const_iterator &start,
    const ActionVector::const_iterator &end) const {
  ActionVector::const_iterator iter = start;
  std::stringstream str;
  for (; iter != end; ++iter) {
    if (iter != start)
      str << ", ";
    str << **iter;
  }
  return str.str();
}