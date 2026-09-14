import js from '@eslint/js';
import globals from 'globals';
import tseslint from 'typescript-eslint';

export default tseslint.config(
    { ignores: ['dist/'] },
    js.configs.recommended,
    tseslint.configs.strictTypeChecked,
    {
        languageOptions: {
            globals: globals.browser,
            parserOptions: { projectService: true, tsconfigRootDir: import.meta.dirname },
        },
        rules: {
            '@typescript-eslint/consistent-type-imports': [
                'error',
                { fixStyle: 'inline-type-imports' },
            ],
            '@typescript-eslint/restrict-template-expressions': ['error', { allowNumber: true }],
        },
    },
    // This file is not part of tsconfig.json, so type-aware rules have nothing to work with.
    { files: ['**/*.js'], extends: [tseslint.configs.disableTypeChecked] },
);
