import nltk
import random
nltk.download('words')
from nltk.corpus import words

five_letter_list = [word for word in words.words() if len(word) == 5]
past_guesses = [] #dont need it, this just a temp to see if it works
final_word = {0:'', 1: '', 2:'', 3: '', 4: ''}

guess = 'tears'
for round in range(0,6):
    result = input('Enter word pattern: ')
    for i, result in enumerate(result):
        print(result)
        if result == 'G' and final_word[i] == '':
            final_word[i] = guess[i]
            five_letter_list = [word for word in five_letter_list if word[i] == guess[i]]
        elif result == 'X':
            five_letter_list = [word for word in five_letter_list if guess[i] not in word]
        elif result == 'Y': #letter is there in the word
            five_letter_list = [word for word in five_letter_list if (word[i] != guess[i] and guess[i] in word)]
    guess = random.choice(five_letter_list)
    print(final_word)
    print(guess)