/* menu_customer.c - the customer's menu. */
#include <stdio.h>
#include <string.h>

#include "client.h"

/* deposit, withdraw and apply for a loan all send just an amount */
static int amount_action(int opcode, const char *prompt)
{
    struct request req;

    memset(&req, 0, sizeof(req));
    req.opcode = opcode;
    if (read_money(prompt, &req.u.amount.amount) == -1)
        return MENU_EXIT;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

static int transfer_action(void)
{
    struct request req;
    int to;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_TRANSFER;
    if (read_int("Receiving account number: ", &to) == -1)
        return MENU_EXIT;
    req.u.transfer.to_account = to;
    if (read_money("Amount to transfer (rupees): ", &req.u.transfer.amount) == -1)
        return MENU_EXIT;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

static int feedback_action(void)
{
    struct request req;
    char text[256];

    memset(&req, 0, sizeof(req));
    req.opcode = OP_ADD_FEEDBACK;
    if (read_line("Your feedback: ", text, sizeof(text)) == -1)
        return MENU_EXIT;
    copy_field(req.u.feedback.message, sizeof(req.u.feedback.message), text);
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

int menu_customer(void)
{
    for (;;) {
        int choice, rc = MENU_STAY;

        printf("\n--- Customer menu ---\n"
               " 1. View account balance\n"
               " 2. Deposit money\n"
               " 3. Withdraw money\n"
               " 4. Transfer funds\n"
               " 5. Apply for a loan\n"
               " 6. Add feedback\n"
               " 7. View transaction history\n"
               " 8. Change password\n"
               " 9. Logout\n"
               " 0. Exit\n");
        if (read_int("Choice: ", &choice) == -1)
            return MENU_EXIT;

        switch (choice) {
        case 1: rc = send_simple(OP_VIEW_BALANCE); break;
        case 2: rc = amount_action(OP_DEPOSIT, "Amount to deposit (rupees): "); break;
        case 3: rc = amount_action(OP_WITHDRAW, "Amount to withdraw (rupees): "); break;
        case 4: rc = transfer_action(); break;
        case 5: rc = amount_action(OP_APPLY_LOAN, "Loan amount (rupees): "); break;
        case 6: rc = feedback_action(); break;
        case 7: rc = send_simple(OP_VIEW_HISTORY); break;
        case 8: rc = action_change_password(); break;
        case 9: return action_logout();
        case 0: return action_exit();
        default: printf("  ! Unknown option.\n"); break;
        }
        if (rc != MENU_STAY)
            return rc;
    }
}
