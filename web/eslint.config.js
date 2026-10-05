import globals from "globals";
import tseslint from "typescript-eslint";
import eslintPluginUnicorn from "eslint-plugin-unicorn";
import eslintConfigPrettier from "eslint-config-prettier/flat";
import eslint from "@eslint/js";

export default tseslint.config(
    {
        ignores: ["out", "node_modules", "dist"]
    },
    {
        files: ["**/*.{ts,tsx}"],
        extends: [
            eslint.configs.recommended,
            tseslint.configs.recommendedTypeChecked,
            tseslint.configs.stylisticTypeChecked,
            eslintPluginUnicorn.configs.recommended,
            eslintConfigPrettier
        ],
        languageOptions: {
            ecmaVersion: 2022,
            globals: globals.builtin,
            parserOptions: {
                projectService: true,
                tsconfigRootDir: import.meta.dirname
            }
        },
        rules: {
            eqeqeq: "error",
            "no-fallthrough": ["error", { allowEmptyCase: true }],
            "@typescript-eslint/no-unused-vars": "error",
            "@typescript-eslint/explicit-member-accessibility": "error",
            "@typescript-eslint/no-deprecated": "error",
            "capitalized-comments": [
                "error",
                "always",
                {
                    ignorePattern: "noinspection|prettier"
                }
            ],
            "@typescript-eslint/no-misused-promises": [
                "error",
                {
                    checksVoidReturn: false
                }
            ],
            // Often used for new Array<type>, more cluttered with Array.from
            "unicorn/no-new-array": "off",

            // I don't like it
            "unicorn/prefer-at": "off"
        }
    },
    {
        files: ["src/render.ts"],
        rules: {
            // CLI entry point: exiting with a message is the interface.
            "unicorn/no-process-exit": "off"
        }
    }
);
