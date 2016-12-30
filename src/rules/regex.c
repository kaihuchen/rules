
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rules.h"
#include "rete.h"
#include "regex.h"

#define REGEX_SYMBOL 0x00
#define REGEX_UNION 0x01
#define REGEX_STAR 0x02
#define REGEX_PLUS 0x03
#define REGEX_QUESTION 0x04
#define REGEX_REGEX 0x05

#define MAX_TRANSITIONS 256
#define MAX_QUEUE 256
#define MAX_STATES 256
#define MAX_LIST 256

#define CREATE_STATE(stateId, newState) do { \
    unsigned int result = createState(stateId, newState); \
    if (result != RULES_OK) { \
        return result; \
    } \
} while (0)

#define LINK_STATES(previousState, nextState, tokenSymbol) do { \
    unsigned int result = linkStates(previousState, nextState, tokenSymbol); \
    if (result != RULES_OK) { \
        return result; \
    } \
} while (0)

#define UNLINK_STATES(previousState, linkIndex) do { \
    unsigned int result = unlinkStates(previousState, linkIndex); \
    if (result != RULES_OK) { \
        return result; \
    } \
} while (0)

#define CREATE_STATE_QUEUE() \
    state *queue[MAX_QUEUE]; \
    unsigned short first = 0; \
    unsigned short last = 0; 

#define ENQUEUE_STATE(value) do { \
    if ((last + 1) == first) { \
        return ERR_REGEX_QUEUE_FULL; \
    } \
    queue[last] = value; \
    last = (last + 1) % MAX_QUEUE; \
} while(0)

#define DEQUEUE_STATE(value) do { \
    if (first == last) { \
        *value = NULL; \
    } else { \
        *value = queue[first]; \
        first = (first + 1) % MAX_QUEUE; \
    } \
} while(0)

#define CREATE_STATE_LIST() \
    state *list[MAX_QUEUE]; \
    unsigned short top = 0;

#define STATE_LIST_EMPTY() !top

#define ADD_STATE(value) do { \
    if ((top + 1) == MAX_LIST) { \
        return ERR_REGEX_LIST_FULL; \
    } \
    list[top++] = value; \
    for (unsigned short i = top - 1; (i > 0) && (list[i]->id < list[i - 1]->id); --i) {\
        state *temp = list[i]; list[i] = list[i - 1]; list[i - 1] = temp; \
    } \
} while(0)

#define ENSURE_STATE(id, newState) do { \
    unsigned int result = ensureState(id, list, top, newState); \
    if (result != RULES_OK) { \
        return result; \
    } \
} while(0)

struct state;

typedef struct transition {
    char symbol;
    struct state *next;
} transition;

typedef struct state {
    unsigned short refCount;
    unsigned short id;
    unsigned short transitionsLength;
    char isAccept;
    transition transitions[MAX_TRANSITIONS];
} state;

typedef struct token {
    unsigned char type;
    char symbol;
} token;

static token empty;

static unsigned int storeRegexStateMachine(ruleset *tree,
                                           unsigned short vocabularyLength, 
                                           unsigned short statesLength,
                                           char **newStateMachine, 
                                           unsigned int *stateMachineOffset) {

    unsigned int stateMachinelength = sizeof(char) * 256;
    stateMachinelength = stateMachinelength + sizeof(unsigned short) * statesLength * vocabularyLength;
    stateMachinelength = stateMachinelength + sizeof(unsigned char) * statesLength;
    if (!tree->regexStateMachinePool) {
        tree->regexStateMachinePool = malloc(stateMachinelength);
        if (!tree->regexStateMachinePool) {
            return ERR_OUT_OF_MEMORY;
        }

        memset(tree->regexStateMachinePool, 0, stateMachinelength);
        *stateMachineOffset = 0;
        *newStateMachine = &tree->regexStateMachinePool[0];
        tree->regexStateMachineOffset = 1;
    } else {
        tree->regexStateMachinePool = realloc(tree->regexStateMachinePool, tree->regexStateMachineOffset + stateMachinelength);
        if (!tree->regexStateMachinePool) {
            return ERR_OUT_OF_MEMORY;
        }

        memset(&tree->regexStateMachinePool[tree->regexStateMachineOffset], 0, stateMachinelength);
        *stateMachineOffset = tree->regexStateMachineOffset;
        *newStateMachine = &tree->regexStateMachinePool[tree->regexStateMachineOffset];
        tree->regexStateMachineOffset = tree->regexStateMachineOffset + stateMachinelength;
    }

    return RULES_OK;
}

static unsigned int createState(unsigned short *stateId, 
                                state **newState) {
    if (*stateId == MAX_STATES) {
        return ERR_REGEX_MAX_STATES;
    }
    *newState = malloc(sizeof(state));
    if (*newState == NULL) {
        return ERR_OUT_OF_MEMORY;
    }
    (*newState)->id = *stateId;
    (*newState)->transitionsLength = 0;
    (*newState)->refCount = 0;
    (*newState)->isAccept = 0;
    ++*stateId;

    return RULES_OK;
}

static unsigned int linkStates(state *previousState, 
                               state *nextState, 
                               char tokenSymbol) {
    previousState->transitions[previousState->transitionsLength].symbol = tokenSymbol;
    previousState->transitions[previousState->transitionsLength].next = nextState;
    ++previousState->transitionsLength;
    ++nextState->refCount;
    if (previousState->transitionsLength == MAX_TRANSITIONS) {
        return ERR_REGEX_MAX_TRANSITIONS;
    }

    return RULES_OK;
}

static unsigned int unlinkStates(state *previousState, unsigned short linkIndex) {
    state *nextState = previousState->transitions[linkIndex].next;
    --nextState->refCount;
    if (!nextState->refCount) {
        free(nextState);
    }

    for (unsigned short i = linkIndex + 1; i < previousState->transitionsLength; ++i) {
        previousState->transitions[i - 1].symbol = previousState->transitions[i].symbol;
        previousState->transitions[i - 1].next = previousState->transitions[i].next;
    }
    --previousState->transitionsLength; 

    return RULES_OK;
}


static unsigned int unescapeSymbol(char **first, 
                                   char *last, 
                                   char *symbol) {
    *first += 2;
    if (*first >= last) {
        return ERR_PARSE_REGEX;
    }

    switch (*first[0]) {
        case '|':
        case '?':
        case '*':
        case '+':
        case '(':
        case ')':
        case '\\':
            *symbol = *first[0];
            return REGEX_PARSE_OK;
    }

    return ERR_PARSE_REGEX;
}

static unsigned int readNextToken(char **first, 
                                  char *last, 
                                  token *nextToken) {
    unsigned int result = REGEX_PARSE_OK;
    if (*first >= last) {
        return REGEX_PARSE_END;
    }

    switch (*first[0]) {
        case '|':
            nextToken->type = REGEX_UNION;
            break;
        case '?':
            nextToken->type = REGEX_QUESTION;
            break;
        case '*':
            nextToken->type = REGEX_STAR;
            break;
        case '+':
            nextToken->type = REGEX_PLUS;
            break;
        case '(':
            nextToken->type = REGEX_REGEX;
            break;
        case ')':
            nextToken->type = REGEX_REGEX;
            result = REGEX_PARSE_END;
            break;
        case '\\':
            nextToken->type = REGEX_SYMBOL;
            result = unescapeSymbol(first, last, &nextToken->symbol);
            if (result != REGEX_PARSE_OK) {
                return result;
            }
            break;
        default:
            nextToken->type = REGEX_SYMBOL;
            nextToken->symbol = *first[0];
    }

    ++*first;
    return result;
}

#ifdef _PRINT
static unsigned int printGraph(state *start) {
    CREATE_STATE_QUEUE();
    unsigned char visited[MAX_STATES] = {0};
    state *currentState = start;
    visited[currentState->id] = 1;
    while (currentState) {
        printf("State %d\n", currentState->id);
        for (int i = 0; i < currentState->transitionsLength; ++ i) {
            transition *currentTransition = &currentState->transitions[i];
            printf("    transition %d to state %d\n", currentTransition->symbol, currentTransition->next->id);
            if (!visited[currentTransition->next->id]) {
                visited[currentTransition->next->id] = 1;
                ENQUEUE_STATE(currentTransition->next);
            }
        }

        DEQUEUE_STATE(&currentState);    
    }

    return RULES_OK;
}
#endif

static unsigned int createGraph(char **first, 
                                char *last, 
                                unsigned short *id, 
                                state **startState, 
                                state **endState) {
    CREATE_STATE(id, startState);
    CREATE_STATE(id, endState);
    state *previousState = *startState;
    state *currentState = *startState;

    token currentToken;
    unsigned int result = readNextToken(first, last, &currentToken);
    while (result == REGEX_PARSE_OK) {
        switch (currentToken.type) {
            case REGEX_SYMBOL:
                previousState = currentState;
                CREATE_STATE(id, &currentState);
                LINK_STATES(previousState, currentState, currentToken.symbol);
                break;
            case REGEX_UNION:
                LINK_STATES(currentState, *endState, empty.symbol);
                previousState = *startState;
                currentState = *startState;
                break;
            case REGEX_STAR:
                {
                    state *newState;
                    CREATE_STATE(id, &newState);
                    LINK_STATES(currentState, previousState, empty.symbol);
                    LINK_STATES(currentState, newState, empty.symbol);
                    LINK_STATES(previousState, newState, empty.symbol);
                    previousState = currentState;
                    currentState = newState;
                }
                break;
            case REGEX_PLUS:
                {
                    state *newState;
                    CREATE_STATE(id, &newState);
                    LINK_STATES(currentState, previousState, empty.symbol);
                    LINK_STATES(currentState, newState, empty.symbol);
                    previousState = currentState;
                    currentState = newState;
                }
                break;
            case REGEX_QUESTION:
                {
                    state *newState;
                    CREATE_STATE(id, &newState);
                    LINK_STATES(currentState, newState, empty.symbol);
                    LINK_STATES(previousState, newState, empty.symbol);
                    previousState = currentState;
                    currentState = newState;
                }
                break;
            case REGEX_REGEX:
                {
                    state *subStart;
                    state *subEnd;
                    result = createGraph(first, last, id, &subStart, &subEnd);
                    if (result != REGEX_PARSE_OK) {
                        return result;
                    }
                    
                    LINK_STATES(currentState, subStart, empty.symbol);
                    previousState = currentState;
                    currentState = subEnd;
                }
                break;
        }
        if (result == REGEX_PARSE_OK) {
            result = readNextToken(first, last, &currentToken);
        }
    }

    LINK_STATES(currentState, *endState, empty.symbol);

    if (result == REGEX_PARSE_END) {
        return REGEX_PARSE_OK;
    }

    return result;
}

static unsigned int validateGraph(char **first, char *last) {
    token currentToken;
    unsigned int result = readNextToken(first, last, &currentToken);
    while (result == REGEX_PARSE_OK) {
        switch (currentToken.type) {
            case REGEX_SYMBOL:
            case REGEX_UNION:
            case REGEX_STAR:
            case REGEX_PLUS:
            case REGEX_QUESTION:
                break;
            case REGEX_REGEX:
                result = validateGraph(first, last);
                if (result != REGEX_PARSE_OK) {
                    return result;
                }
                    
                break;
        }

        if (result == REGEX_PARSE_OK) {
            result = readNextToken(first, last, &currentToken);
        }
    }

    if (result == REGEX_PARSE_END) {
        return REGEX_PARSE_OK;
    }

    return REGEX_PARSE_OK;
}

static unsigned int ensureState(unsigned short *id, 
                                state **list, 
                                unsigned short stateListLength, 
                                state **newState) {
    CREATE_STATE(id, newState);
    for (unsigned short i = 0; i < stateListLength; ++i) {
        state *targetState = list[i];
        for (unsigned short ii = 0; ii < targetState->transitionsLength; ++ii) {
            transition *targetTransition = &targetState->transitions[ii];
            LINK_STATES(*newState, targetTransition->next, targetTransition->symbol);
        }

        if (targetState->id == 1) {
            (*newState)->isAccept = 1;
        }

        if (!targetState->refCount) {
            free(targetState);
        }
    }

    return RULES_OK;
}

static unsigned int consolidateStates(state *currentState, 
                                      unsigned short *id) {
    for (unsigned short i = 0; i < currentState->transitionsLength; ++i) {
        transition *currentTransition = &currentState->transitions[i];
        if (!currentTransition->symbol) {
            state *nextState = currentTransition->next;
            if (nextState != currentState) {
                for (unsigned short ii = 0; ii < nextState->transitionsLength; ++ii) {
                    transition *nextTransition = &nextState->transitions[ii];
                    LINK_STATES(currentState, nextTransition->next, nextTransition->symbol);
                    if (nextState->refCount == 1) {
                        --nextTransition->next->refCount;
                    }
                }
            }

            if (nextState->id == 1) {
                currentState->isAccept = 1;
            }

            UNLINK_STATES(currentState, i);
            --i;
        }
    }

    return RULES_OK;
}

static unsigned int consolidateTransitions(state *currentState, 
                                           unsigned short *id) {
    for (unsigned short i = 0; i < currentState->transitionsLength; ++i) {
        transition *currentTransition = &currentState->transitions[i];
        CREATE_STATE_LIST();
        for (unsigned short ii = i + 1; ii < currentState->transitionsLength; ++ ii) {
            transition *targetTransition = &currentState->transitions[ii];
            if (currentTransition->symbol == targetTransition->symbol) {
                if (currentTransition->next != targetTransition->next) {
                    if (STATE_LIST_EMPTY()) {
                        ADD_STATE(currentTransition->next);
                        UNLINK_STATES(currentState, i);
                    }

                    ADD_STATE(targetTransition->next);
                }
                UNLINK_STATES(currentState, ii);
            }
        }

        if (!STATE_LIST_EMPTY()) {
            state *newState;
            ENSURE_STATE(id, &newState);
            LINK_STATES(currentState, newState, currentTransition->symbol);
        }
    }

    return RULES_OK;
}

static unsigned int transformToDFA(state *nfa, 
                                   unsigned short *id) {
    CREATE_STATE_QUEUE();
    unsigned char visited[MAX_STATES] = {0};
    state *currentState = nfa;
    visited[currentState->id] = 1;
    while (currentState) {
        unsigned int result = consolidateStates(currentState, id);
        if (result != RULES_OK) {
            return result;
        }

        result = consolidateTransitions(currentState, id);
        if (result != RULES_OK) {
            return result;
        }

        for (int i = 0; i < currentState->transitionsLength; ++ i) {
            transition *currentTransition = &currentState->transitions[i];
            if (!visited[currentTransition->next->id]) {
                visited[currentTransition->next->id] = 1;
                ENQUEUE_STATE(currentTransition->next);
            }
        }

        DEQUEUE_STATE(&currentState);    
    }

    return RULES_OK;
}

static unsigned int calculateGraphDimensions(state *start, 
                                        unsigned short *vocabularyLength, 
                                        unsigned short *statesLength) {
    *vocabularyLength = 0;
    *statesLength = 0;
    CREATE_STATE_QUEUE();
    unsigned char visited[MAX_STATES] = {0};
    unsigned char vocabulary[sizeof(char) * 256] = {0};
    state *currentState = start;
    visited[currentState->id] = 1;
    while (currentState) {
        ++*statesLength;
        for (int i = 0; i < currentState->transitionsLength; ++ i) {
            transition *currentTransition = &currentState->transitions[i];
            if (!vocabulary[(unsigned short)currentTransition->symbol]) {
                vocabulary[(unsigned short)currentTransition->symbol] = 1;
                ++*vocabularyLength;
            }

            if (!visited[currentTransition->next->id]) {
                visited[currentTransition->next->id] = 1;
                ENQUEUE_STATE(currentTransition->next);
            }
        }

        DEQUEUE_STATE(&currentState);    
    }

    return RULES_OK;

}

static unsigned int packGraph(state *start, 
                              char *stateMachine,
                              unsigned short vocabularyLength,
                              unsigned short statesLength) {

    CREATE_STATE_QUEUE();
    unsigned short visited[MAX_STATES] = {0};
    char *vocabulary = stateMachine;
    unsigned short *stateTable = (unsigned short *)(stateMachine + 256);
    unsigned char *acceptVector = (unsigned char *)(stateTable + (vocabularyLength * statesLength));
    unsigned short stateNumber = 1;
    unsigned short vocabularyNumber = 1;
    state *currentState = start;
    visited[currentState->id] = stateNumber;
    ++stateNumber;
    while (currentState) {
        unsigned short targetStateNumber = visited[currentState->id];
        if (currentState->isAccept) {
            acceptVector[targetStateNumber - 1] = 1;
        }

        for (int i = 0; i < currentState->transitionsLength; ++ i) {
            transition *currentTransition = &currentState->transitions[i];
            if (!vocabulary[(unsigned short)currentTransition->symbol]) {
                vocabulary[(unsigned short)currentTransition->symbol] = vocabularyNumber;
                ++vocabularyNumber;
            }

            if (!visited[currentTransition->next->id]) {
                visited[currentTransition->next->id] = stateNumber;
                ++stateNumber;
                ENQUEUE_STATE(currentTransition->next);
            }

            unsigned short targetSymbolNumber = vocabulary[(unsigned short)currentTransition->symbol];
            stateTable[(targetSymbolNumber - 1) * statesLength + (targetStateNumber - 1)] = visited[currentTransition->next->id];
        }

        DEQUEUE_STATE(&currentState);    
    }

    return RULES_OK;
}

unsigned int validateRegex(char *first, 
                           char *last) {
    return validateGraph(&first, last);
}

unsigned int compileRegex(void *tree, 
                          char *first, 
                          char *last, 
                          unsigned short *vocabularyLength,
                          unsigned short *statesLength,
                          unsigned int *regexStateMachineOffset) {
    state *start;
    state *end;
    unsigned short id = 0;
    unsigned int result = createGraph(&first, last, &id, &start, &end);
    if (result != RULES_OK) {
        return result;
    }

    ++start->refCount;
    result = transformToDFA(start, &id);
    if (result != RULES_OK) {
        return result;
    }

#ifdef _PRINT
    printGraph(start);
#endif
    
    result = calculateGraphDimensions(start, 
                                 vocabularyLength, 
                                 statesLength);
    if (result != RULES_OK) {
        return result;
    }

    char *newStateMachine;    
    result = storeRegexStateMachine((ruleset *)tree, 
                                    *vocabularyLength, 
                                    *statesLength,
                                    &newStateMachine,
                                    regexStateMachineOffset);
    if (result != RULES_OK) {
        return result;
    }

    return packGraph(start, 
                     newStateMachine, 
                     *vocabularyLength,
                     *statesLength);
}

unsigned char evaluateRegex(void *tree,
                            char *first,
                            unsigned short length, 
                            unsigned short vocabularyLength,
                            unsigned short statesLength,
                            unsigned int regexStateMachineOffset) {
    char *vocabulary = &((ruleset *)tree)->regexStateMachinePool[regexStateMachineOffset];
    unsigned short *stateTable = (unsigned short *)(vocabulary + 256);
    unsigned char *acceptVector = (unsigned char *)(stateTable + (vocabularyLength * statesLength));
    unsigned short currentState = 1;
    for (int i = 0; i < length; ++i) {
        char currentSymbol = vocabulary[(unsigned short) first[i]];
        if (!currentSymbol) {
            return 0;
        }

        currentState = stateTable[statesLength * (currentSymbol - 1) + (currentState - 1)];
        if (!currentState) {
            return 0;
        }
    }

    return acceptVector[currentState - 1];
}