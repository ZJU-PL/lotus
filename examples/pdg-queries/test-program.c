/**
 * @file test-program.c
 * @brief Test program for demonstrating PDG query language
 *
 * This program contains various patterns that can be analyzed using
 * the PDG query language, including information flows, access control,
 * and data dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global variables for testing
char* secret_data = "secret123";
int user_authenticated = 0;
char* user_input = NULL;

// Function to get user input
char* getInput() {
    char buffer[256];
    printf("Enter input: ");
    if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        user_input = malloc(strlen(buffer) + 1);
        strcpy(user_input, buffer);
        return user_input;
    }
    return NULL;
}

// Function to get secret data
char* getSecret() {
    return secret_data;
}

// Function to check authentication
int isAuthorized() {
    return user_authenticated;
}

// Function to sanitize input
char* sanitize(char* input) {
    if (input == NULL) return NULL;
    
    char* sanitized = malloc(strlen(input) + 1);
    int j = 0;
    for (int i = 0; input[i] != '\0'; i++) {
        if (input[i] != '<' && input[i] != '>' && input[i] != '&') {
            sanitized[j++] = input[i];
        }
    }
    sanitized[j] = '\0';
    return sanitized;
}

// Function to print output
void printOutput(char* message) {
    if (message != NULL) {
        printf("Output: %s\n", message);
    }
}

// Function to send data over network
void networkSend(char* data) {
    if (data != NULL) {
        printf("Sending over network: %s\n", data);
    }
}

// Function to log data
void logData(char* data) {
    if (data != NULL) {
        printf("Logging: %s\n", data);
    }
}

// Main function with various patterns
int main() {
    char* input = getInput();
    char* secret = getSecret();
    
    // Pattern 1: Direct information flow (should be detected)
    if (input != NULL) {
        printOutput(input);  // Direct flow from input to output
    }
    
    // Pattern 2: Controlled access to secret
    if (isAuthorized()) {
        printOutput(secret);  // Secret only printed if authorized
    }
    
    // Pattern 3: Sanitized flow
    char* sanitized_input = sanitize(input);
    if (sanitized_input != NULL) {
        networkSend(sanitized_input);  // Sanitized input sent over network
    }
    
    // Pattern 4: Uncontrolled secret flow (security issue)
    if (input != NULL && strcmp(input, "admin") == 0) {
        networkSend(secret);  // Secret leaked if input is "admin"
    }
    
    // Pattern 5: Logging sensitive data
    logData(secret);  // Secret always logged (potential issue)
    
    // Pattern 6: Complex control flow
    int authorized = isAuthorized();
    if (authorized) {
        char* processed = malloc(strlen(secret) + 1);
        strcpy(processed, secret);
        printOutput(processed);
        free(processed);
    }
    
    // Cleanup
    if (input != NULL) {
        free(input);
    }
    if (sanitized_input != NULL) {
        free(sanitized_input);
    }
    
    return 0;
}

// Additional functions for testing
void processUserData(char* data) {
    if (data != NULL) {
        // Process the data
        char* processed = malloc(strlen(data) + 10);
        sprintf(processed, "Processed: %s", data);
        printOutput(processed);
        free(processed);
    }
}

void handleAdminRequest(char* request) {
    if (isAuthorized()) {
        if (request != NULL) {
            networkSend(request);
        }
    }
}

// Function with multiple return paths
char* getData(int type) {
    if (type == 1) {
        return getInput();
    } else if (type == 2) {
        return getSecret();
    } else {
        return "default";
    }
}
