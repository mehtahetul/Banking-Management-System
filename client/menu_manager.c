/* menu_manager.c - the manager's menu. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "client.h"

static int set_status_action(void)
{
    struct request req;
    char answer[16];
    int account_id;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_SET_ACCOUNT_STATUS;
    if (read_int("Account number: ", &account_id) == -1) return MENU_EXIT;
    if (read_line("Activate or deactivate? (a/d): ", answer, sizeof(answer)) == -1) return MENU_EXIT;

    if (tolower((unsigned char)answer[0]) == 'a')
        req.u.set_status.status = ACC_ACTIVE;
    else if (tolower((unsigned char)answer[0]) == 'd')
        req.u.set_status.status = ACC_DEACTIVATED;
    else {
        printf("  ! Please type a (activate) or d (deactivate).\n");
        return MENU_STAY;
    }
    req.u.set_status.account_id = account_id;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

static int assign_loan_action(void)
{
    struct request req;
    int loan_id, employee_id;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_ASSIGN_LOAN;
    if (read_int("Loan id: ", &loan_id) == -1) return MENU_EXIT;
    if (read_int("Employee's user id: ", &employee_id) == -1) return MENU_EXIT;
    req.u.assign_loan.loan_id = loan_id;
    req.u.assign_loan.employee_id = employee_id;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

int menu_manager(void)
{
    for (;;) {
        int choice, rc = MENU_STAY;

        printf("\n--- Manager menu ---\n"
               " 1. Activate / deactivate a customer account\n"
               " 2. Assign a loan application to an employee\n"
               " 3. Review customer feedback\n"
               " 4. Change password\n"
               " 5. Logout\n"
               " 0. Exit\n");
        if (read_int("Choice: ", &choice) == -1)
            return MENU_EXIT;

        switch (choice) {
        case 1: rc = set_status_action(); break;
        case 2: rc = assign_loan_action(); break;
        case 3: rc = send_simple(OP_REVIEW_FEEDBACK); break;
        case 4: rc = action_change_password(); break;
        case 5: return action_logout();
        case 0: return action_exit();
        default: printf("  ! Unknown option.\n"); break;
        }
        if (rc != MENU_STAY)
            return rc;
    }
}
