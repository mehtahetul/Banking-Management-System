/* menu_employee.c - the bank employee's menu. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "client.h"

static int add_customer_action(void)
{
    struct request req;
    char username[32], password[128], name[50], phone[16], email[50];

    memset(&req, 0, sizeof(req));
    req.opcode = OP_ADD_CUSTOMER;
    if (read_line("Username: ", username, sizeof(username)) == -1) return MENU_EXIT;
    if (read_password("Password: ", password, sizeof(password)) == -1) return MENU_EXIT;
    if (read_line("Full name: ", name, sizeof(name)) == -1) return MENU_EXIT;
    if (read_line("Phone: ", phone, sizeof(phone)) == -1) return MENU_EXIT;
    if (read_line("Email: ", email, sizeof(email)) == -1) return MENU_EXIT;

    copy_field(req.u.new_user.username, sizeof(req.u.new_user.username), username);
    copy_field(req.u.new_user.password, sizeof(req.u.new_user.password), password);
    copy_field(req.u.new_user.name, sizeof(req.u.new_user.name), name);
    copy_field(req.u.new_user.phone, sizeof(req.u.new_user.phone), phone);
    copy_field(req.u.new_user.email, sizeof(req.u.new_user.email), email);
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

static int modify_customer_action(void)
{
    struct request req;
    char name[50], phone[16], email[50];
    int user_id;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_MODIFY_CUSTOMER;
    if (read_int("Customer's user id: ", &user_id) == -1) return MENU_EXIT;
    printf("Leave a field blank to keep its current value.\n");
    if (read_line("New name: ", name, sizeof(name)) == -1) return MENU_EXIT;
    if (read_line("New phone: ", phone, sizeof(phone)) == -1) return MENU_EXIT;
    if (read_line("New email: ", email, sizeof(email)) == -1) return MENU_EXIT;

    req.u.modify_user.user_id = user_id;
    copy_field(req.u.modify_user.name, sizeof(req.u.modify_user.name), name);
    copy_field(req.u.modify_user.phone, sizeof(req.u.modify_user.phone), phone);
    copy_field(req.u.modify_user.email, sizeof(req.u.modify_user.email), email);
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

static int process_loan_action(void)
{
    struct request req;
    char answer[16];
    int loan_id;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_PROCESS_LOAN;
    if (read_int("Loan id: ", &loan_id) == -1) return MENU_EXIT;
    if (read_line("Approve or reject? (a/r): ", answer, sizeof(answer)) == -1) return MENU_EXIT;

    if (tolower((unsigned char)answer[0]) == 'a')
        req.u.process_loan.approve = 1;
    else if (tolower((unsigned char)answer[0]) == 'r')
        req.u.process_loan.approve = 0;
    else {
        printf("  ! Please type a (approve) or r (reject).\n");
        return MENU_STAY;
    }
    req.u.process_loan.loan_id = loan_id;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

static int passbook_action(void)
{
    struct request req;
    int account_id;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_VIEW_PASSBOOK;
    if (read_int("Customer's account number: ", &account_id) == -1) return MENU_EXIT;
    req.u.account.account_id = account_id;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

int menu_employee(void)
{
    for (;;) {
        int choice, rc = MENU_STAY;

        printf("\n--- Employee menu ---\n"
               " 1. Add new customer\n"
               " 2. Modify customer details\n"
               " 3. View my assigned loan applications\n"
               " 4. Approve / reject a loan\n"
               " 5. View a customer's passbook\n"
               " 6. Change password\n"
               " 7. Logout\n"
               " 0. Exit\n");
        if (read_int("Choice: ", &choice) == -1)
            return MENU_EXIT;

        switch (choice) {
        case 1: rc = add_customer_action(); break;
        case 2: rc = modify_customer_action(); break;
        case 3: rc = send_simple(OP_VIEW_ASSIGNED_LOANS); break;
        case 4: rc = process_loan_action(); break;
        case 5: rc = passbook_action(); break;
        case 6: rc = action_change_password(); break;
        case 7: return action_logout();
        case 0: return action_exit();
        default: printf("  ! Unknown option.\n"); break;
        }
        if (rc != MENU_STAY)
            return rc;
    }
}
