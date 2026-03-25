NAME = ft_ping

CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2
LDFLAGS = -lm  # Link math library for sqrt()

SRCS = ft_ping.c
OBJS = $(SRCS:.c=.o)

HEADERS = ft_ping.h

# Colors
GREEN = \033[0;32m
RED = \033[0;31m
RESET = \033[0m

# Default target
all: $(NAME)

# Link object files to create executable
$(NAME): $(OBJS)
	@echo "$(GREEN)Linking $(NAME)...$(RESET)"
	$(CC) $(OBJS) -o $(NAME) $(LDFLAGS)
	chmod +x $(NAME)
	@echo "$(GREEN)$(NAME) created successfully!$(RESET)"
	@echo "$(RED)Note: Run with sudo or as root for raw socket access$(RESET)"

# Compile source files to object files
%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Clean object files
clean:
	@echo "$(RED)Cleaning object files...$(RESET)"
	rm -f $(OBJS)

# Clean everything
fclean: clean
	@echo "$(RED)Removing $(NAME)...$(RESET)"
	rm -f $(NAME)

# Rebuild everything
re: fclean all

# Set capabilities (alternative to running as root)
setcap: $(NAME)
	@echo "$(GREEN)Setting capabilities for $(NAME)...$(RESET)"
	@echo "This allows running without sudo"
	sudo setcap cap_net_raw+ep $(NAME)

# Remove capabilities
rmcap: $(NAME)
	@echo "$(RED)Removing capabilities from $(NAME)...$(RESET)"
	sudo setcap -r $(NAME) 2>/dev/null || true

# Run basic test
test: $(NAME)
	@echo "$(GREEN)Testing $(NAME) with localhost...$(RESET)"
	sudo ./$(NAME) localhost

# Run verbose test
test-verbose: $(NAME)
	@echo "$(GREEN)Testing $(NAME) with verbose mode...$(RESET)"
	sudo ./$(NAME) -v 127.0.0.1

# Show help
help: $(NAME)
	./$(NAME) -?


# Run with valgrind
valgrind: $(NAME)
	@echo "$(GREEN)Running with valgrind...$(RESET)"
	sudo valgrind --leak-check=full --show-leak-kinds=all ./$(NAME) localhost -c 4


.PHONY: all clean fclean re setcap rmcap test test-verbose help valgrind