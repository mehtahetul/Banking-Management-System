/* menu_admin.c - the administrator's menu. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "client.h"

static int add_employee_action(void)
{
    struct request req;
    char username[32], password[128], name[50], phone[16], email[50];

    memset(&req, 0, sizeof(req));
    req.opcode = OP_ADD_EMPLOYEE;
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

static int modify_user_action(void)
{
    struct request req;
    char name[50], phone[16], email[50];
    int user_id;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_MODIFY_USER;
    if (read_int("User id (customer or employee): ", &user_id) == -1) return MENU_EXIT;
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

static int set_role_action(void)
{
    struct request req;
    char answer[16];
    int user_id;

    memset(&req, 0, sizeof(req));
    req.opcode = OP_SET_ROLE;
    if (read_int("User id: ", &user_id) == -1) return MENU_EXIT;
    if (read_line("New role: e = employee, m = manager: ", answer, sizeof(answer)) == -1)
        return MENU_EXIT;

    if (tolower((unsigned char)answer[0]) == 'e')
        req.u.set_role.role = ROLE_EMPLOYEE;
    else if (tolower((unsigned char)answer[0]) == 'm')
        req.u.set_role.role = ROLE_MANAGER;
    else {
        printf("  ! Please type e (employee) or m (manager).\n");
        return MENU_STAY;
    }
    req.u.set_role.user_id = user_id;
    if (call_server(&req, NULL) == -1)
        return MENU_LOST;
    return MENU_STAY;
}

int menu_admin(void)
{
    for (;;) {
        int choice, rc = MENU_STAY;

        printf("\n--- Administrator menu ---\n"
               " 1. Add new bank employee\n"
               " 2. Modify customer / employee details\n"
               " 3. Manage user roles (employee <-> manager)\n"
               " 4. Change password\n"
               " 5. Logout\n"
               " 0. Exit\n");
        if (read_int("Choice: ", &choice) == -1)
            return MENU_EXIT;

        switch (choice) {
        case 1: rc = add_employee_action(); break;
        case 2: rc = modify_user_action(); break;
        case 3: rc = set_role_action(); break;
        case 4: rc = action_change_password(); break;
        case 5: return action_logout();
        case 0: return action_exit();
        default: printf("  ! Unknown option.\n"); break;
        }
        if (rc != MENU_STAY)
            return rc;
    }
}
